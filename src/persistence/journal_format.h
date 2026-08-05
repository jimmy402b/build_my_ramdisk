#pragma once

#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/persistence/file_identity.h"
#include "src/persistence/persistence_types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace myrd::persistence {

using JournalHeaderBytes = std::array<std::byte, kJournalAlignment>;

inline constexpr std::array<std::byte, 8> kJournalHeaderMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'R'}, std::byte{'D'}, std::byte{'J'}, std::byte{0}};

inline constexpr std::uint64_t kJournalMandatoryFeatureMask =
    0xFFFFFFFF00000000ULL;

inline constexpr std::array<std::byte, 8> kJournalRecordMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'R'}, std::byte{'C'}, std::byte{'R'}, std::byte{0}};
inline constexpr std::uint64_t kDataRecordBytes =
    kJournalAlignment + kM2BlockSize;
inline constexpr std::uint64_t kFixedRecordBytes = kJournalAlignment;
inline constexpr std::uint64_t kJournalMandatoryRecordFlags =
    0xFFFFFFFF00000000ULL;

enum class JournalRecordType : std::uint16_t {
    Data = 1,
    Zero = 2,
    Discard = 3,
    IndexNode = 4,
    CheckpointCommit = 5,
};

struct JournalRecordCommon final {
    JournalRecordType type{JournalRecordType::Data};
    CheckpointGeneration checkpoint_generation{0};
    std::uint64_t operation_id{0};
    std::uint64_t sequence{0};
    std::uint64_t flags{0};

    bool operator==(const JournalRecordCommon&) const noexcept = default;
};

struct BlockJournalRecord final {
    JournalRecordCommon common{};
    std::uint64_t block_index{0};
    std::uint64_t snapshot_version{0};
    BaseGeneration source_generation{0};
    std::uint64_t dirty_epoch{0};

    bool operator==(const BlockJournalRecord&) const noexcept = default;
};

struct DecodedBlockJournalRecord final {
    BlockJournalRecord metadata{};
    std::uint32_t logical_payload_length{0};
    std::uint32_t payload_crc32c{0};
    std::span<const std::byte> payload{};
};

inline constexpr std::uint16_t kJournalIndexEntryBytes = 48;
inline constexpr std::uint16_t kJournalIndexMaxEntries = 82;

struct JournalIndexLeafEntry final {
    std::uint64_t block_index{0};
    JournalRecordType record_type{JournalRecordType::Data};
    std::uint32_t payload_crc32c{0};
    std::uint64_t record_offset{0};
    std::uint64_t record_length{0};
    CheckpointGeneration record_generation{0};

    bool operator==(const JournalIndexLeafEntry&) const noexcept = default;
};

struct JournalIndexInternalEntry final {
    std::uint64_t minimum_key{0};
    std::uint64_t maximum_key{0};
    std::uint64_t child_offset{0};
    std::uint64_t child_length{0};
    std::uint32_t child_crc32c{0};
    CheckpointGeneration child_generation{0};

    bool operator==(const JournalIndexInternalEntry&) const noexcept = default;
};

struct JournalIndexNode final {
    JournalRecordCommon common{JournalRecordType::IndexNode};
    std::uint16_t level{0};
    std::vector<JournalIndexLeafEntry> leaf_entries{};
    std::vector<JournalIndexInternalEntry> internal_entries{};

    bool operator==(const JournalIndexNode&) const noexcept = default;
};

struct DecodedJournalIndexNode final {
    JournalIndexNode node{};
    std::uint64_t minimum_key{0};
    std::uint64_t maximum_key{0};
    std::uint32_t record_crc32c{0};
};

struct CheckpointCommitRecord final {
    JournalRecordCommon common{JournalRecordType::CheckpointCommit};
    ImageUuid image_uuid{};
    JournalFileIdentity journal_identity{};
    JournalEpoch journal_epoch{0};
    CheckpointGeneration previous_generation{0};
    CheckpointGeneration new_generation{0};
    Sha256Digest previous_digest{};
    Sha256Digest new_chain_digest{};
    std::uint64_t data_count{0};
    std::uint64_t zero_count{0};
    std::uint64_t discard_count{0};
    std::uint64_t index_node_count{0};
    PersistentIndexRootDescriptor root{};
    std::uint64_t checkpoint_begin_offset{0};
    std::uint64_t journal_committed_end{0};
    Sha256Digest geometry_digest{};
    std::uint64_t input_record_count{0};

    bool operator==(const CheckpointCommitRecord&) const noexcept = default;
};

struct CheckpointDigestContext final {
    Sha256Digest previous_digest{};
    ImageUuid image_uuid{};
    JournalEpoch journal_epoch{0};
    CheckpointGeneration previous_generation{0};
    CheckpointGeneration new_generation{0};
    Sha256Digest geometry_digest{};
};

struct JournalHeader;

class CheckpointChainDigest final {
public:
    static Result<CheckpointChainDigest> begin(
        const CheckpointDigestContext& context);

    CheckpointChainDigest(CheckpointChainDigest&&) noexcept;
    CheckpointChainDigest& operator=(CheckpointChainDigest&&) noexcept;
    ~CheckpointChainDigest();

    CheckpointChainDigest(const CheckpointChainDigest&) = delete;
    CheckpointChainDigest& operator=(const CheckpointChainDigest&) = delete;

    Result<void> append_non_commit_record(
        std::span<const std::byte> record_bytes);
    Result<Sha256Digest> finalize(
        std::span<const std::byte> commit_bytes);

private:
    struct Impl;
    explicit CheckpointChainDigest(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

class CheckpointSequenceValidator final {
public:
    static Result<CheckpointSequenceValidator> begin(
        const JournalHeader& header,
        CheckpointGeneration previous_generation,
        CheckpointGeneration new_generation,
        const Sha256Digest& previous_digest,
        std::uint64_t checkpoint_begin_offset,
        std::uint64_t committed_bound,
        std::uint64_t max_input_records);

    CheckpointSequenceValidator(CheckpointSequenceValidator&&) noexcept;
    CheckpointSequenceValidator& operator=(
        CheckpointSequenceValidator&&) noexcept;
    ~CheckpointSequenceValidator();

    CheckpointSequenceValidator(const CheckpointSequenceValidator&) = delete;
    CheckpointSequenceValidator& operator=(
        const CheckpointSequenceValidator&) = delete;

    Result<void> append(std::span<const std::byte> record_bytes,
                        std::uint64_t record_offset);
    Result<CheckpointCommitRecord> finalize();

private:
    struct Impl;
    explicit CheckpointSequenceValidator(
        std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

enum class JournalPredecessorKind : std::uint32_t {
    Genesis = 0,
    Checkpoint = 1,
};

struct JournalHeader final {
    ImageUuid image_uuid{};
    JournalFileIdentity journal_identity{};
    JournalEpoch epoch{0};
    ImageGeometry geometry{};
    CheckpointGeneration base_checkpoint_generation{0};
    JournalPredecessorKind predecessor_kind{
        JournalPredecessorKind::Genesis};
    JournalEpoch predecessor_epoch{0};
    JournalFileIdentity predecessor_journal_identity{};
    Sha256Digest predecessor_digest{};
    Sha256Digest geometry_digest{};
    Sha256Digest base_identity_digest{};
    std::uint64_t feature_flags{0};

    bool operator==(const JournalHeader& other) const noexcept {
        return image_uuid == other.image_uuid &&
               journal_identity == other.journal_identity &&
               epoch == other.epoch &&
               geometry.virtual_bytes == other.geometry.virtual_bytes &&
               geometry.logical_sector_size ==
                   other.geometry.logical_sector_size &&
               geometry.physical_sector_size ==
                   other.geometry.physical_sector_size &&
               base_checkpoint_generation ==
                   other.base_checkpoint_generation &&
               predecessor_kind == other.predecessor_kind &&
               predecessor_epoch == other.predecessor_epoch &&
               predecessor_journal_identity ==
                   other.predecessor_journal_identity &&
               predecessor_digest == other.predecessor_digest &&
               geometry_digest == other.geometry_digest &&
               base_identity_digest == other.base_identity_digest &&
               feature_flags == other.feature_flags;
    }
};

[[nodiscard]] Result<Sha256Digest> compute_geometry_digest(
    const ImageGeometry& geometry);

[[nodiscard]] Result<Sha256Digest> compute_base_identity_digest(
    const VhdxFileIdentity& identity);

[[nodiscard]] Result<Sha256Digest> compute_genesis_digest(
    const ImageUuid& image_uuid,
    const Sha256Digest& geometry_digest,
    const Sha256Digest& base_identity_digest);

[[nodiscard]] Result<JournalHeaderBytes> encode_journal_header(
    const JournalHeader& header);

[[nodiscard]] Result<JournalHeader> decode_journal_header(
    std::span<const std::byte> bytes);

[[nodiscard]] Result<std::uint32_t> journal_block_logical_length(
    const ImageGeometry& geometry, std::uint64_t block_index);

[[nodiscard]] Result<std::vector<std::byte>> encode_block_record(
    const BlockJournalRecord& record,
    std::span<const std::byte> payload,
    const ImageGeometry& geometry);

[[nodiscard]] Result<DecodedBlockJournalRecord> decode_block_record(
    std::span<const std::byte> bytes,
    std::uint64_t record_offset,
    std::uint64_t committed_bound,
    const ImageGeometry& geometry);

[[nodiscard]] Result<std::array<std::byte, kIndexNodeBytes>>
encode_index_node(const JournalIndexNode& node,
                  std::uint64_t record_offset,
                  std::uint64_t committed_bound);

[[nodiscard]] Result<DecodedJournalIndexNode> decode_index_node(
    std::span<const std::byte> bytes,
    std::uint64_t record_offset,
    std::uint64_t committed_bound);

[[nodiscard]] Result<std::array<std::byte, kJournalAlignment>>
encode_checkpoint_commit(const CheckpointCommitRecord& commit,
                         const JournalHeader& header,
                         std::uint64_t record_offset,
                         std::uint64_t committed_bound);

[[nodiscard]] Result<CheckpointCommitRecord> decode_checkpoint_commit(
    std::span<const std::byte> bytes,
    const JournalHeader& header,
    std::uint64_t record_offset,
    std::uint64_t committed_bound);

}  // namespace myrd::persistence
