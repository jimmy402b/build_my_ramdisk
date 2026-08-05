#include "test_framework.h"

#include "src/common/sha256_stream.h"
#include "src/persistence/journal_format.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::persistence {
namespace {

template <typename Buffer>
void put_u16(Buffer& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

template <typename Buffer>
void put_u32(Buffer& bytes, std::size_t offset,
             std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::byte>(
            (value >> (i * 8U)) & 0xFFU);
    }
}

template <typename Buffer>
void put_u64(Buffer& bytes, std::size_t offset,
             std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<std::byte>(
            (value >> (i * 8U)) & 0xFFU);
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

void refresh_header_crc(JournalHeaderBytes& bytes) {
    put_u32(bytes, 20, 0);
    put_u32(bytes, 20, independent_crc32c(bytes));
}

template <typename Buffer>
void refresh_record_crc(Buffer& bytes) {
    put_u32(bytes, 32, 0);
    put_u32(bytes, 32, independent_crc32c(bytes));
}

void require_block_rejected(std::span<const std::byte> bytes,
                            std::uint64_t offset,
                            std::uint64_t committed_bound,
                            const ImageGeometry& geometry) {
    const auto decoded = decode_block_record(
        bytes, offset, committed_bound, geometry);
    REQUIRE(!decoded.ok());
    REQUIRE_EQ(decoded.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(decoded.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::InvalidFormat));
}

void require_header_rejected(std::span<const std::byte> bytes) {
    const auto decoded = decode_journal_header(bytes);
    REQUIRE(!decoded.ok());
    REQUIRE_EQ(decoded.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(decoded.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::InvalidFormat));
}

void require_header_rejected_with_message(
    std::span<const std::byte> bytes, std::wstring_view message) {
    const auto decoded = decode_journal_header(bytes);
    REQUIRE(!decoded.ok());
    REQUIRE_EQ(decoded.error().message, message);
}

Sha256Digest independent_digest(
    std::initializer_list<std::span<const std::byte>> chunks) {
    Sha256Stream stream;
    for (const auto chunk : chunks) REQUIRE(stream.update(chunk).ok());
    const auto digest = stream.finalize_digest();
    REQUIRE(digest.ok());
    return digest.value();
}

template <std::size_t Size>
std::span<const std::byte> literal_bytes(const char (&text)[Size]) {
    return {reinterpret_cast<const std::byte*>(text), Size - 1};
}

std::array<std::byte, 8> le64(std::uint64_t value) {
    std::array<std::byte, 8> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
    return bytes;
}

std::array<std::byte, 4> le32(std::uint32_t value) {
    std::array<std::byte, 4> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = static_cast<std::byte>((value >> (i * 8U)) & 0xFFU);
    }
    return bytes;
}

ImageGeometry sample_geometry() {
    return {64ULL * 1024 * 1024, 512, 4096};
}

Sha256Digest independent_geometry_digest(const ImageGeometry& geometry) {
    const auto virtual_bytes = le64(geometry.virtual_bytes);
    const auto logical = le32(geometry.logical_sector_size);
    const auto physical = le32(geometry.physical_sector_size);
    const auto m2 = le64(kM2BlockSize);
    const auto vhdx = le32(kVhdxDataBlockBytes);
    return independent_digest({
        literal_bytes("MYRD-GEOMETRY-V1"), virtual_bytes, logical,
        physical, m2, vhdx});
}

VhdxFileIdentity sample_base_identity() {
    VhdxFileIdentity identity{};
    for (std::size_t i = 0; i < 16; ++i) {
        identity.file_write_guid[i] = static_cast<std::byte>(0x30U + i);
        identity.data_write_guid[i] = static_cast<std::byte>(0x40U + i);
    }
    return identity;
}

Sha256Digest independent_base_digest(const VhdxFileIdentity& identity) {
    return independent_digest({
        literal_bytes("MYRD-BASE-ID-V1"), identity.file_write_guid,
        identity.data_write_guid});
}

JournalHeader sample_header() {
    JournalHeader header{};
    for (std::size_t i = 0; i < 16; ++i) {
        header.image_uuid.bytes[i] = static_cast<std::byte>(0x10U + i);
        header.journal_identity.journal_uuid[i] =
            static_cast<std::byte>(0x20U + i);
    }
    header.epoch = 1;
    header.geometry = sample_geometry();
    header.base_checkpoint_generation = 0;
    header.predecessor_kind = JournalPredecessorKind::Genesis;
    header.predecessor_epoch = 0;
    header.geometry_digest = independent_geometry_digest(header.geometry);
    header.base_identity_digest = independent_base_digest(sample_base_identity());
    const auto generation_zero = le64(0);
    header.predecessor_digest = independent_digest({
        literal_bytes("MYRD-GENESIS-V1"), header.image_uuid.bytes,
        header.geometry_digest, generation_zero,
        header.base_identity_digest});
    header.feature_flags = 1;
    return header;
}

JournalHeaderBytes independent_header_golden() {
    const auto header = sample_header();
    JournalHeaderBytes bytes{};
    const std::array<std::byte, 8> magic{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'R'}, std::byte{'D'}, std::byte{'J'}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put_u32(bytes, 8, 1);
    put_u32(bytes, 12, 0);
    put_u32(bytes, 16, 4096);
    std::copy(header.image_uuid.bytes.begin(), header.image_uuid.bytes.end(),
              bytes.begin() + 24);
    std::copy(header.journal_identity.journal_uuid.begin(),
              header.journal_identity.journal_uuid.end(), bytes.begin() + 40);
    put_u64(bytes, 56, header.epoch);
    put_u64(bytes, 64, header.geometry.virtual_bytes);
    put_u32(bytes, 72, header.geometry.logical_sector_size);
    put_u32(bytes, 76, header.geometry.physical_sector_size);
    put_u32(bytes, 80, static_cast<std::uint32_t>(kM2BlockSize));
    put_u32(bytes, 84, kVhdxDataBlockBytes);
    put_u64(bytes, 88, header.base_checkpoint_generation);
    put_u32(bytes, 96, 0);
    put_u64(bytes, 104, header.predecessor_epoch);
    std::copy(header.predecessor_journal_identity.journal_uuid.begin(),
              header.predecessor_journal_identity.journal_uuid.end(),
              bytes.begin() + 112);
    std::copy(header.predecessor_digest.begin(),
              header.predecessor_digest.end(), bytes.begin() + 128);
    std::copy(header.geometry_digest.begin(), header.geometry_digest.end(),
              bytes.begin() + 160);
    std::copy(header.base_identity_digest.begin(),
              header.base_identity_digest.end(), bytes.begin() + 192);
    put_u64(bytes, 224, header.feature_flags);
    put_u32(bytes, 20, independent_crc32c(bytes));
    return bytes;
}

int hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    return -1;
}

JournalHeaderBytes load_sparse_header_fixture() {
    std::ifstream input{MYRD_M3_JOURNAL_HEADER_FIXTURE_PATH};
    REQUIRE(input.good());
    JournalHeaderBytes bytes{};
    std::array<bool, kJournalAlignment> assigned{};
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

std::vector<std::byte> load_sparse_fixture(const char* path,
                                           std::size_t size) {
    std::ifstream input{path};
    REQUIRE(input.good());
    std::vector<std::byte> bytes(size);
    std::vector<bool> assigned(size);
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

Sha256Digest load_digest_fixture() {
    std::ifstream input{MYRD_M3_JOURNAL_DIGEST_FIXTURE_PATH};
    REQUIRE(input.good());
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty() || line[0] == '#') continue;
        REQUIRE_EQ(line.size(), 64ull);
        Sha256Digest digest{};
        for (std::size_t i = 0; i < digest.size(); ++i) {
            const auto high = hex_nibble(line[i * 2]);
            const auto low = hex_nibble(line[i * 2 + 1]);
            REQUIRE(high >= 0);
            REQUIRE(low >= 0);
            digest[i] = static_cast<std::byte>((high << 4) | low);
        }
        return digest;
    }
    REQUIRE(false);
    return {};
}

BlockJournalRecord sample_block_record(JournalRecordType type) {
    BlockJournalRecord record{};
    record.common.type = type;
    record.common.checkpoint_generation = 5;
    record.common.operation_id = 0x1122;
    record.common.sequence = type == JournalRecordType::Data
        ? 0
        : (type == JournalRecordType::Zero ? 1 : 2);
    record.common.flags = 1;
    record.block_index = type == JournalRecordType::Data
        ? 3
        : (type == JournalRecordType::Zero ? 4 : 5);
    record.snapshot_version = type == JournalRecordType::Data
        ? 9
        : (type == JournalRecordType::Zero ? 10 : 11);
    record.source_generation = 4;
    record.dirty_epoch = type == JournalRecordType::Data
        ? 7
        : (type == JournalRecordType::Zero ? 8 : 9);
    return record;
}

std::vector<std::byte> sample_data_payload() {
    std::vector<std::byte> payload(kM2BlockSize);
    payload[0] = std::byte{0xAA};
    payload[32768] = std::byte{0xBB};
    payload[65535] = std::byte{0xCC};
    return payload;
}

std::vector<std::byte> independent_block_golden(
    const BlockJournalRecord& record,
    std::span<const std::byte> payload,
    const ImageGeometry& geometry) {
    const bool data = record.common.type == JournalRecordType::Data;
    const auto length = data
        ? kJournalAlignment + kM2BlockSize
        : kJournalAlignment;
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    const std::array<std::byte, 8> magic{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'R'}, std::byte{'C'}, std::byte{'R'}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put_u32(bytes, 8, 1);
    put_u32(bytes, 12, 0);
    put_u32(bytes, 16, 4096);
    put_u16(bytes, 20, static_cast<std::uint16_t>(record.common.type));
    put_u64(bytes, 24, length);
    put_u64(bytes, 40, record.common.checkpoint_generation);
    put_u64(bytes, 48, record.common.operation_id);
    put_u64(bytes, 56, record.common.sequence);
    put_u64(bytes, 64, record.common.flags);
    put_u64(bytes, 72, record.block_index);
    put_u64(bytes, 80, record.snapshot_version);
    put_u64(bytes, 88, record.source_generation);
    put_u64(bytes, 96, record.dirty_epoch);
    if (data) {
        const auto block_start = record.block_index * kM2BlockSize;
        const auto remaining = geometry.virtual_bytes - block_start;
        const auto logical = static_cast<std::uint32_t>(
            (std::min)(remaining, kM2BlockSize));
        put_u32(bytes, 104, logical);
        put_u32(bytes, 108, static_cast<std::uint32_t>(kM2BlockSize));
        put_u32(bytes, 112, independent_crc32c(payload));
        std::copy(payload.begin(), payload.end(),
                  bytes.begin() + kJournalAlignment);
    }
    put_u32(bytes, 32, independent_crc32c(bytes));
    return bytes;
}

JournalIndexNode sample_leaf_node(std::uint32_t data_payload_crc) {
    JournalIndexNode node{};
    node.common.type = JournalRecordType::IndexNode;
    node.common.checkpoint_generation = 5;
    node.common.operation_id = 0x1122;
    node.common.sequence = 3;
    node.common.flags = 1;
    node.level = 0;
    node.leaf_entries = {
        {3, JournalRecordType::Data, data_payload_crc, 0x1000,
         kDataRecordBytes, 5},
        {4, JournalRecordType::Zero, 0, 0x12000,
         kFixedRecordBytes, 5},
        {5, JournalRecordType::Discard, 0, 0x13000,
         kFixedRecordBytes, 5}};
    return node;
}

std::array<std::byte, kIndexNodeBytes> independent_index_golden(
    const JournalIndexNode& node) {
    std::array<std::byte, kIndexNodeBytes> bytes{};
    const std::array<std::byte, 8> magic{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'R'}, std::byte{'C'}, std::byte{'R'}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put_u32(bytes, 8, 1);
    put_u32(bytes, 12, 0);
    put_u32(bytes, 16, 4096);
    put_u16(bytes, 20, 4);
    put_u64(bytes, 24, 4096);
    put_u64(bytes, 40, node.common.checkpoint_generation);
    put_u64(bytes, 48, node.common.operation_id);
    put_u64(bytes, 56, node.common.sequence);
    put_u64(bytes, 64, node.common.flags);
    put_u16(bytes, 72, node.level);
    put_u16(bytes, 74,
            static_cast<std::uint16_t>(node.leaf_entries.size()));
    put_u16(bytes, 76, 0);
    put_u16(bytes, 78, 48);
    put_u64(bytes, 80, node.leaf_entries.front().block_index);
    put_u64(bytes, 88, node.leaf_entries.back().block_index);
    put_u64(bytes, 96, node.common.checkpoint_generation);
    for (std::size_t i = 0; i < node.leaf_entries.size(); ++i) {
        const auto& entry = node.leaf_entries[i];
        const auto base = 128 + i * 48;
        put_u64(bytes, base, entry.block_index);
        put_u16(bytes, base + 8,
                static_cast<std::uint16_t>(entry.record_type));
        put_u32(bytes, base + 12, entry.payload_crc32c);
        put_u64(bytes, base + 16, entry.record_offset);
        put_u64(bytes, base + 24, entry.record_length);
        put_u64(bytes, base + 32, entry.record_generation);
    }
    put_u32(bytes, 32, independent_crc32c(bytes));
    return bytes;
}

CheckpointCommitRecord sample_commit(std::uint32_t root_crc,
                                     const Sha256Digest& digest) {
    const auto header = sample_header();
    CheckpointCommitRecord commit{};
    commit.common.type = JournalRecordType::CheckpointCommit;
    commit.common.checkpoint_generation = 5;
    commit.common.operation_id = 0x1122;
    commit.common.sequence = 4;
    commit.common.flags = 1;
    commit.image_uuid = header.image_uuid;
    commit.journal_identity = header.journal_identity;
    commit.journal_epoch = header.epoch;
    commit.previous_generation = 4;
    commit.new_generation = 5;
    for (std::size_t i = 0; i < commit.previous_digest.size(); ++i) {
        commit.previous_digest[i] = static_cast<std::byte>(0xD0U + i);
    }
    commit.new_chain_digest = digest;
    commit.data_count = 1;
    commit.zero_count = 1;
    commit.discard_count = 1;
    commit.index_node_count = 1;
    commit.root = {0x14000, kIndexNodeBytes, root_crc};
    commit.checkpoint_begin_offset = 0x1000;
    commit.journal_committed_end = 0x16000;
    commit.geometry_digest = header.geometry_digest;
    commit.input_record_count = 4;
    return commit;
}

std::array<std::byte, kJournalAlignment> independent_commit_golden(
    const CheckpointCommitRecord& commit) {
    std::array<std::byte, kJournalAlignment> bytes{};
    const std::array<std::byte, 8> magic{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'R'}, std::byte{'C'}, std::byte{'R'}, std::byte{0}};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    put_u32(bytes, 8, 1);
    put_u32(bytes, 16, 4096);
    put_u16(bytes, 20, 5);
    put_u64(bytes, 24, 4096);
    put_u64(bytes, 40, commit.common.checkpoint_generation);
    put_u64(bytes, 48, commit.common.operation_id);
    put_u64(bytes, 56, commit.common.sequence);
    put_u64(bytes, 64, commit.common.flags);
    std::copy(commit.image_uuid.bytes.begin(), commit.image_uuid.bytes.end(),
              bytes.begin() + 72);
    std::copy(commit.journal_identity.journal_uuid.begin(),
              commit.journal_identity.journal_uuid.end(),
              bytes.begin() + 88);
    put_u64(bytes, 104, commit.journal_epoch);
    put_u64(bytes, 112, commit.previous_generation);
    put_u64(bytes, 120, commit.new_generation);
    std::copy(commit.previous_digest.begin(), commit.previous_digest.end(),
              bytes.begin() + 128);
    std::copy(commit.new_chain_digest.begin(),
              commit.new_chain_digest.end(), bytes.begin() + 160);
    put_u64(bytes, 192, commit.data_count);
    put_u64(bytes, 200, commit.zero_count);
    put_u64(bytes, 208, commit.discard_count);
    put_u64(bytes, 216, commit.index_node_count);
    put_u64(bytes, 224, commit.root.offset);
    put_u64(bytes, 232, commit.root.length);
    put_u32(bytes, 240, commit.root.crc32c);
    put_u64(bytes, 248, commit.checkpoint_begin_offset);
    put_u64(bytes, 256, commit.journal_committed_end);
    std::copy(commit.geometry_digest.begin(), commit.geometry_digest.end(),
              bytes.begin() + 264);
    put_u64(bytes, 296, commit.input_record_count);
    put_u32(bytes, 32, independent_crc32c(bytes));
    return bytes;
}

Sha256Digest independent_checkpoint_digest(
    const CheckpointCommitRecord& commit,
    const std::vector<std::span<const std::byte>>& records,
    std::array<std::byte, kJournalAlignment> commit_bytes) {
    put_u32(commit_bytes, 32, 0);
    std::fill_n(commit_bytes.begin() + 160, 32, std::byte{0});
    const auto epoch = le64(commit.journal_epoch);
    const auto previous_generation = le64(commit.previous_generation);
    const auto new_generation = le64(commit.new_generation);
    Sha256Stream stream;
    for (const auto prefix : {
             literal_bytes("MYRD-CHECKPOINT-V1"),
             std::span<const std::byte>{commit.previous_digest},
             std::span<const std::byte>{commit.image_uuid.bytes},
             std::span<const std::byte>{epoch},
             std::span<const std::byte>{previous_generation},
             std::span<const std::byte>{new_generation},
             std::span<const std::byte>{commit.geometry_digest}}) {
        REQUIRE(stream.update(prefix).ok());
    }
    for (const auto record : records) REQUIRE(stream.update(record).ok());
    REQUIRE(stream.update(commit_bytes).ok());
    const auto digest = stream.finalize_digest();
    REQUIRE(digest.ok());
    return digest.value();
}

std::uint32_t get_u32(std::span<const std::byte> bytes,
                      std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + i]))
                 << (i * 8U);
    }
    return value;
}

struct SampleCheckpoint final {
    std::vector<std::byte> data;
    std::vector<std::byte> zero;
    std::vector<std::byte> discard;
    JournalIndexNode node;
    std::array<std::byte, kIndexNodeBytes> index{};
    CheckpointCommitRecord commit;
    std::array<std::byte, kJournalAlignment> commit_bytes{};
    Sha256Digest digest{};
};

SampleCheckpoint independent_sample_checkpoint() {
    SampleCheckpoint sample{};
    const auto geometry = sample_geometry();
    const auto payload = sample_data_payload();
    sample.data = independent_block_golden(
        sample_block_record(JournalRecordType::Data), payload, geometry);
    sample.zero = independent_block_golden(
        sample_block_record(JournalRecordType::Zero), {}, geometry);
    sample.discard = independent_block_golden(
        sample_block_record(JournalRecordType::Discard), {}, geometry);
    sample.node = sample_leaf_node(get_u32(sample.data, 112));
    sample.index = independent_index_golden(sample.node);
    Sha256Digest placeholder{};
    placeholder[0] = std::byte{1};
    sample.commit = sample_commit(get_u32(sample.index, 32), placeholder);
    sample.commit_bytes = independent_commit_golden(sample.commit);
    const std::vector<std::span<const std::byte>> records{
        sample.data, sample.zero, sample.discard, sample.index};
    sample.digest = independent_checkpoint_digest(
        sample.commit, records, sample.commit_bytes);
    sample.commit.new_chain_digest = sample.digest;
    sample.commit_bytes = independent_commit_golden(sample.commit);
    return sample;
}

JournalIndexNode sample_internal_node() {
    JournalIndexNode node{};
    node.common.type = JournalRecordType::IndexNode;
    node.common.checkpoint_generation = 6;
    node.common.operation_id = 0x3344;
    node.common.sequence = 0;
    node.level = 1;
    node.internal_entries = {
        {1, 10, 0x1000, kIndexNodeBytes, 0x11111111, 5},
        {11, 20, 0x2000, kIndexNodeBytes, 0x22222222, 6}};
    return node;
}

CheckpointCommitRecord sample_genesis_commit() {
    const auto header = sample_header();
    CheckpointCommitRecord commit{};
    commit.common.type = JournalRecordType::CheckpointCommit;
    commit.common.checkpoint_generation = 0;
    commit.common.operation_id = 1;
    commit.common.sequence = 0;
    commit.image_uuid = header.image_uuid;
    commit.journal_identity = header.journal_identity;
    commit.journal_epoch = 1;
    commit.previous_generation = 0;
    commit.new_generation = 0;
    commit.previous_digest = header.predecessor_digest;
    commit.new_chain_digest = header.predecessor_digest;
    commit.checkpoint_begin_offset = kJournalAlignment;
    commit.journal_committed_end = 2 * kJournalAlignment;
    commit.geometry_digest = header.geometry_digest;
    return commit;
}

void require_index_rejected(std::span<const std::byte> bytes,
                            std::uint64_t offset = 0x14000,
                            std::uint64_t bound = 0x16000) {
    const auto decoded = decode_index_node(bytes, offset, bound);
    REQUIRE(!decoded.ok());
}

void require_commit_rejected(std::span<const std::byte> bytes,
                             std::uint64_t offset = 0x15000,
                             std::uint64_t bound = 0x16000) {
    const auto decoded = decode_checkpoint_commit(
        bytes, sample_header(), offset, bound);
    REQUIRE(!decoded.ok());
}

TEST_CASE(journal_v1_independent_header_builder_matches_frozen_fixture) {
    REQUIRE_EQ(independent_header_golden(), load_sparse_header_fixture());
}

TEST_CASE(journal_v1_header_encoder_matches_independent_golden) {
    const auto encoded = encode_journal_header(sample_header());
    REQUIRE(encoded.ok());
    REQUIRE_EQ(encoded.value(), independent_header_golden());
}

TEST_CASE(journal_v1_header_decoder_reads_independent_golden) {
    const auto decoded = decode_journal_header(independent_header_golden());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), sample_header());
}

TEST_CASE(journal_v1_digest_helpers_match_independent_oracle) {
    const auto header = sample_header();
    REQUIRE_EQ(compute_geometry_digest(header.geometry).value(),
               header.geometry_digest);
    REQUIRE_EQ(compute_base_identity_digest(sample_base_identity()).value(),
               header.base_identity_digest);
    REQUIRE_EQ(compute_genesis_digest(
                   header.image_uuid, header.geometry_digest,
                   header.base_identity_digest).value(),
               header.predecessor_digest);
}

TEST_CASE(journal_v1_header_crc_covers_every_physical_byte) {
    const auto golden = independent_header_golden();
    for (std::size_t offset = 0; offset < golden.size(); ++offset) {
        auto corrupt = golden;
        corrupt[offset] ^= std::byte{1};
        if (offset < 8) {
            require_header_rejected_with_message(
                corrupt, L"journal header magic mismatch");
        } else if ((offset >= 8 && offset < 12) ||
                   (offset >= 16 && offset < 20)) {
            require_header_rejected_with_message(
                corrupt, L"unsupported journal header version");
        } else {
            require_header_rejected_with_message(
                corrupt, L"journal header CRC32C mismatch");
        }
    }
}

TEST_CASE(journal_v1_header_reserved_bytes_are_individually_rejected) {
    for (std::size_t offset = 100; offset < 104; ++offset) {
        auto corrupt = independent_header_golden();
        corrupt[offset] = std::byte{1};
        refresh_header_crc(corrupt);
        require_header_rejected_with_message(
            corrupt, L"journal reserved bytes are nonzero");
    }
    for (std::size_t offset = 232; offset < kJournalAlignment; ++offset) {
        auto corrupt = independent_header_golden();
        corrupt[offset] = std::byte{1};
        refresh_header_crc(corrupt);
        require_header_rejected_with_message(
            corrupt, L"journal reserved bytes are nonzero");
    }
}

TEST_CASE(journal_v1_header_rejects_every_truncation_and_extra_byte) {
    const auto golden = independent_header_golden();
    for (std::size_t size = 0; size < golden.size(); ++size) {
        require_header_rejected(
            std::span<const std::byte>{golden.data(), size});
    }
    std::array<std::byte, kJournalAlignment + 1> extra{};
    std::copy(golden.begin(), golden.end(), extra.begin());
    require_header_rejected(extra);
}

TEST_CASE(journal_v1_header_rejects_invalid_identity_generation_and_flags) {
    auto header = sample_header();
    header.image_uuid = {};
    REQUIRE(!encode_journal_header(header).ok());

    header = sample_header();
    header.journal_identity = {};
    REQUIRE(!encode_journal_header(header).ok());

    header = sample_header();
    header.journal_identity.journal_uuid = header.image_uuid.bytes;
    REQUIRE(!encode_journal_header(header).ok());

    for (const auto epoch : {std::uint64_t{0}, kInvalidGeneration}) {
        header = sample_header();
        header.epoch = epoch;
        REQUIRE(!encode_journal_header(header).ok());
    }

    header = sample_header();
    header.base_checkpoint_generation = kInvalidGeneration;
    REQUIRE(!encode_journal_header(header).ok());

    header = sample_header();
    header.geometry.virtual_bytes = 0;
    REQUIRE(!encode_journal_header(header).ok());

    header = sample_header();
    header.feature_flags = 1ULL << 63U;
    REQUIRE(!encode_journal_header(header).ok());

    header = sample_header();
    header.predecessor_digest[0] ^= std::byte{1};
    REQUIRE(!encode_journal_header(header).ok());
}

TEST_CASE(journal_v1_checkpoint_predecessor_round_trips) {
    auto header = sample_header();
    header.epoch = 2;
    header.base_checkpoint_generation = 9;
    header.predecessor_kind = JournalPredecessorKind::Checkpoint;
    header.predecessor_epoch = 1;
    for (std::size_t i = 0; i < 16; ++i) {
        header.predecessor_journal_identity.journal_uuid[i] =
            static_cast<std::byte>(0x70U + i);
    }
    for (std::size_t i = 0; i < header.predecessor_digest.size(); ++i) {
        header.predecessor_digest[i] = static_cast<std::byte>(0x80U + i);
    }
    const auto encoded = encode_journal_header(header);
    REQUIRE(encoded.ok());
    const auto decoded = decode_journal_header(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), header);
}

TEST_CASE(journal_v1_cross_epoch_predecessor_rules_fail_closed) {
    auto header = sample_header();
    header.epoch = 3;
    header.base_checkpoint_generation = 9;
    header.predecessor_kind = JournalPredecessorKind::Checkpoint;
    header.predecessor_epoch = 2;
    for (std::size_t i = 0; i < 16; ++i) {
        header.predecessor_journal_identity.journal_uuid[i] =
            static_cast<std::byte>(0x70U + i);
    }
    for (std::size_t i = 0; i < header.predecessor_digest.size(); ++i) {
        header.predecessor_digest[i] = static_cast<std::byte>(0x80U + i);
    }
    REQUIRE(encode_journal_header(header).ok());

    auto invalid = header;
    invalid.predecessor_epoch = 1;
    REQUIRE(!encode_journal_header(invalid).ok());
    invalid = header;
    invalid.predecessor_journal_identity = header.journal_identity;
    REQUIRE(!encode_journal_header(invalid).ok());
    invalid = header;
    invalid.predecessor_journal_identity = {};
    REQUIRE(!encode_journal_header(invalid).ok());
    invalid = header;
    invalid.predecessor_digest = {};
    REQUIRE(!encode_journal_header(invalid).ok());
}

TEST_CASE(journal_v1_later_minor_with_only_optional_features_is_readable) {
    auto bytes = independent_header_golden();
    put_u32(bytes, 12, 7);
    refresh_header_crc(bytes);
    REQUIRE(decode_journal_header(bytes).ok());
}

TEST_CASE(journal_v1_block_builders_match_frozen_fixtures) {
    const auto geometry = sample_geometry();
    const auto payload = sample_data_payload();
    REQUIRE_EQ(independent_block_golden(
                   sample_block_record(JournalRecordType::Data),
                   payload, geometry),
               load_sparse_fixture(MYRD_M3_JOURNAL_DATA_FIXTURE_PATH,
                                   kJournalAlignment + kM2BlockSize));
    REQUIRE_EQ(independent_block_golden(
                   sample_block_record(JournalRecordType::Zero), {}, geometry),
               load_sparse_fixture(MYRD_M3_JOURNAL_ZERO_FIXTURE_PATH,
                                   kJournalAlignment));
    REQUIRE_EQ(independent_block_golden(
                   sample_block_record(JournalRecordType::Discard), {}, geometry),
               load_sparse_fixture(MYRD_M3_JOURNAL_DISCARD_FIXTURE_PATH,
                                   kJournalAlignment));
}

TEST_CASE(journal_v1_block_encoders_match_independent_golden) {
    const auto geometry = sample_geometry();
    const auto payload = sample_data_payload();
    for (const auto type : {JournalRecordType::Data,
                            JournalRecordType::Zero,
                            JournalRecordType::Discard}) {
        const auto record = sample_block_record(type);
        const auto encoded = encode_block_record(
            record, type == JournalRecordType::Data
                        ? std::span<const std::byte>{payload}
                        : std::span<const std::byte>{},
            geometry);
        REQUIRE(encoded.ok());
        REQUIRE_EQ(encoded.value(), independent_block_golden(
            record, type == JournalRecordType::Data
                        ? std::span<const std::byte>{payload}
                        : std::span<const std::byte>{},
            geometry));
    }
}

TEST_CASE(journal_v1_block_decoders_read_independent_golden) {
    const auto geometry = sample_geometry();
    const auto payload = sample_data_payload();
    std::uint64_t offset = kJournalAlignment;
    for (const auto type : {JournalRecordType::Data,
                            JournalRecordType::Zero,
                            JournalRecordType::Discard}) {
        const auto expected = sample_block_record(type);
        const auto bytes = independent_block_golden(
            expected, type == JournalRecordType::Data
                          ? std::span<const std::byte>{payload}
                          : std::span<const std::byte>{},
            geometry);
        const auto decoded = decode_block_record(
            bytes, offset, offset + bytes.size(), geometry);
        REQUIRE(decoded.ok());
        REQUIRE_EQ(decoded.value().metadata, expected);
        if (type == JournalRecordType::Data) {
            REQUIRE_EQ(decoded.value().payload.size(), kM2BlockSize);
            REQUIRE(std::equal(decoded.value().payload.begin(),
                               decoded.value().payload.end(), payload.begin()));
        } else {
            REQUIRE(decoded.value().payload.empty());
        }
        offset += bytes.size();
    }
}

TEST_CASE(journal_v1_block_record_crc_detects_header_body_and_payload_damage) {
    const auto geometry = sample_geometry();
    const auto payload = sample_data_payload();
    const auto golden = independent_block_golden(
        sample_block_record(JournalRecordType::Data), payload, geometry);
    for (const auto offset : {std::size_t{0}, std::size_t{8},
                              std::size_t{20}, std::size_t{32},
                              std::size_t{72}, std::size_t{115},
                              std::size_t{4096},
                              golden.size() - 1}) {
        auto corrupt = golden;
        corrupt[offset] ^= std::byte{1};
        require_block_rejected(corrupt, kJournalAlignment,
                               kJournalAlignment + corrupt.size(), geometry);
    }
}

TEST_CASE(journal_v1_block_record_reserved_bytes_are_individually_rejected) {
    const auto geometry = sample_geometry();
    const auto golden = independent_block_golden(
        sample_block_record(JournalRecordType::Zero), {}, geometry);
    std::vector<std::size_t> offsets{22, 23, 36, 37, 38, 39};
    for (std::size_t offset = 116; offset < kJournalAlignment; ++offset) {
        offsets.push_back(offset);
    }
    for (const auto offset : offsets) {
        auto corrupt = golden;
        corrupt[offset] = std::byte{1};
        refresh_record_crc(corrupt);
        require_block_rejected(corrupt, kJournalAlignment,
                               2 * kJournalAlignment, geometry);
    }
}

TEST_CASE(journal_v1_block_records_reject_all_truncations_and_extra_bytes) {
    const auto geometry = sample_geometry();
    const auto payload = sample_data_payload();
    for (const auto type : {JournalRecordType::Data,
                            JournalRecordType::Zero,
                            JournalRecordType::Discard}) {
        const auto golden = independent_block_golden(
            sample_block_record(type),
            type == JournalRecordType::Data
                ? std::span<const std::byte>{payload}
                : std::span<const std::byte>{},
            geometry);
        for (std::size_t size = 0; size < golden.size(); ++size) {
            require_block_rejected(
                std::span<const std::byte>{golden.data(), size},
                kJournalAlignment, kJournalAlignment + golden.size(),
                geometry);
        }
        auto extra = golden;
        extra.push_back(std::byte{0});
        require_block_rejected(extra, kJournalAlignment,
                               kJournalAlignment + extra.size(), geometry);
    }
}

TEST_CASE(journal_v1_block_records_validate_partial_final_blocks) {
    for (const auto geometry : {
             ImageGeometry{kM2BlockSize + 512, 512, 4096},
             ImageGeometry{kM2BlockSize + 4096, 4096, 4096},
             ImageGeometry{512, 512, 4096}}) {
        auto record = sample_block_record(JournalRecordType::Data);
        record.block_index = geometry.virtual_bytes > kM2BlockSize ? 1 : 0;
        std::vector<std::byte> payload(kM2BlockSize);
        const auto logical = journal_block_logical_length(
            geometry, record.block_index);
        REQUIRE(logical.ok());
        REQUIRE_EQ(logical.value(),
                   static_cast<std::uint32_t>(
                       geometry.virtual_bytes > kM2BlockSize
                           ? geometry.virtual_bytes - kM2BlockSize
                           : geometry.virtual_bytes));
        const auto encoded = encode_block_record(record, payload, geometry);
        REQUIRE(encoded.ok());
        const auto decoded = decode_block_record(
            encoded.value(), kJournalAlignment,
            kJournalAlignment + encoded.value().size(), geometry);
        REQUIRE(decoded.ok());
        REQUIRE_EQ(decoded.value().logical_payload_length, logical.value());

        payload[logical.value()] = std::byte{1};
        REQUIRE(!encode_block_record(record, payload, geometry).ok());

        auto corrupt = encoded.value();
        corrupt[kJournalAlignment + logical.value()] = std::byte{1};
        put_u32(corrupt, 112, independent_crc32c(
            std::span<const std::byte>{corrupt}.subspan(
                kJournalAlignment, kM2BlockSize)));
        refresh_record_crc(corrupt);
        require_block_rejected(corrupt, kJournalAlignment,
                               kJournalAlignment + corrupt.size(), geometry);
    }
}

TEST_CASE(journal_v1_block_records_reject_invalid_metadata_and_ranges) {
    const auto geometry = sample_geometry();
    const auto payload = sample_data_payload();
    auto record = sample_block_record(JournalRecordType::Data);

    for (const auto generation : {std::uint64_t{0}, kInvalidGeneration}) {
        record = sample_block_record(JournalRecordType::Data);
        record.common.checkpoint_generation = generation;
        REQUIRE(!encode_block_record(record, payload, geometry).ok());
    }
    record = sample_block_record(JournalRecordType::Data);
    record.common.operation_id = 0;
    REQUIRE(!encode_block_record(record, payload, geometry).ok());
    record = sample_block_record(JournalRecordType::Data);
    record.common.flags = 1ULL << 63U;
    REQUIRE(!encode_block_record(record, payload, geometry).ok());
    record = sample_block_record(JournalRecordType::Data);
    record.snapshot_version = 0;
    REQUIRE(!encode_block_record(record, payload, geometry).ok());
    record = sample_block_record(JournalRecordType::Data);
    record.dirty_epoch = 0;
    REQUIRE(!encode_block_record(record, payload, geometry).ok());
    record = sample_block_record(JournalRecordType::Data);
    record.source_generation = record.common.checkpoint_generation;
    REQUIRE(!encode_block_record(record, payload, geometry).ok());
    record = sample_block_record(JournalRecordType::Data);
    record.block_index = geometry.virtual_bytes / kM2BlockSize;
    REQUIRE(!encode_block_record(record, payload, geometry).ok());
    record.block_index = UINT64_MAX;
    REQUIRE(!encode_block_record(record, payload, geometry).ok());

    const auto golden = independent_block_golden(
        sample_block_record(JournalRecordType::Data), payload, geometry);
    require_block_rejected(golden, 0, UINT64_MAX, geometry);
    require_block_rejected(golden, kJournalAlignment + 1,
                           UINT64_MAX, geometry);
    require_block_rejected(golden, UINT64_MAX - kJournalAlignment + 1,
                           UINT64_MAX, geometry);
    require_block_rejected(golden, kJournalAlignment,
                           kJournalAlignment + golden.size() - 1, geometry);
}

TEST_CASE(journal_v1_block_decoder_rejects_semantic_mutations_with_valid_crc) {
    const auto geometry = sample_geometry();
    const auto payload = sample_data_payload();
    const auto golden = independent_block_golden(
        sample_block_record(JournalRecordType::Data), payload, geometry);
    const std::vector<std::pair<std::size_t, std::uint64_t>> mutations{
        {20, 4}, {24, kJournalAlignment}, {40, 0}, {48, 0},
        {64, 1ULL << 63U}, {72, UINT64_MAX}, {80, 0},
        {88, 5}, {96, 0}, {104, 1}, {108, 1}};
    for (const auto& [offset, value] : mutations) {
        auto corrupt = golden;
        if (offset == 20) {
            put_u16(corrupt, offset, static_cast<std::uint16_t>(value));
        } else if (offset == 104 || offset == 108) {
            put_u32(corrupt, offset, static_cast<std::uint32_t>(value));
        } else {
            put_u64(corrupt, offset, value);
        }
        refresh_record_crc(corrupt);
        require_block_rejected(corrupt, kJournalAlignment,
                               kJournalAlignment + corrupt.size(), geometry);
    }

    for (const auto type : {JournalRecordType::Zero,
                            JournalRecordType::Discard}) {
        auto corrupt = independent_block_golden(
            sample_block_record(type), {}, geometry);
        put_u32(corrupt, 104, 1);
        refresh_record_crc(corrupt);
        require_block_rejected(corrupt, kJournalAlignment,
                               2 * kJournalAlignment, geometry);
    }
}

TEST_CASE(journal_v1_index_commit_and_digest_match_frozen_fixtures) {
    const auto sample = independent_sample_checkpoint();
    const auto index_fixture = load_sparse_fixture(
        MYRD_M3_JOURNAL_INDEX_FIXTURE_PATH, kIndexNodeBytes);
    REQUIRE(std::equal(sample.index.begin(), sample.index.end(),
                       index_fixture.begin(), index_fixture.end()));
    const auto commit_fixture = load_sparse_fixture(
        MYRD_M3_JOURNAL_COMMIT_FIXTURE_PATH, kJournalAlignment);
    REQUIRE(std::equal(sample.commit_bytes.begin(),
                       sample.commit_bytes.end(),
                       commit_fixture.begin(), commit_fixture.end()));
    REQUIRE_EQ(sample.digest, load_digest_fixture());
}

TEST_CASE(journal_v1_index_codec_matches_independent_bytes) {
    const auto sample = independent_sample_checkpoint();
    const auto encoded = encode_index_node(
        sample.node, 0x14000, 0x16000);
    REQUIRE(encoded.ok());
    REQUIRE_EQ(encoded.value(), sample.index);
    const auto decoded = decode_index_node(
        sample.index, 0x14000, 0x16000);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().node, sample.node);
    REQUIRE_EQ(decoded.value().minimum_key, 3ull);
    REQUIRE_EQ(decoded.value().maximum_key, 5ull);
    REQUIRE_EQ(decoded.value().record_crc32c,
               get_u32(sample.index, 32));
}

TEST_CASE(journal_v1_commit_codec_matches_independent_bytes) {
    const auto sample = independent_sample_checkpoint();
    const auto header = sample_header();
    const auto encoded = encode_checkpoint_commit(
        sample.commit, header, 0x15000, 0x16000);
    REQUIRE(encoded.ok());
    REQUIRE_EQ(encoded.value(), sample.commit_bytes);
    const auto decoded = decode_checkpoint_commit(
        sample.commit_bytes, header, 0x15000, 0x16000);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), sample.commit);
}

TEST_CASE(journal_v1_streaming_digest_matches_independent_oracle) {
    const auto sample = independent_sample_checkpoint();
    CheckpointDigestContext context{
        sample.commit.previous_digest,
        sample.commit.image_uuid,
        sample.commit.journal_epoch,
        sample.commit.previous_generation,
        sample.commit.new_generation,
        sample.commit.geometry_digest};
    auto stream = CheckpointChainDigest::begin(context);
    REQUIRE(stream.ok());
    for (const auto record : {
             std::span<const std::byte>{sample.data},
             std::span<const std::byte>{sample.zero},
             std::span<const std::byte>{sample.discard},
             std::span<const std::byte>{sample.index}}) {
        REQUIRE(stream.value().append_non_commit_record(record).ok());
    }
    const auto digest = stream.value().finalize(sample.commit_bytes);
    REQUIRE(digest.ok());
    REQUIRE_EQ(digest.value(), sample.digest);
}

TEST_CASE(journal_v1_sequence_validator_accepts_complete_checkpoint) {
    const auto sample = independent_sample_checkpoint();
    auto validator = CheckpointSequenceValidator::begin(
        sample_header(), sample.commit.previous_generation,
        sample.commit.new_generation, sample.commit.previous_digest,
        0x1000, 0x16000, 4);
    REQUIRE(validator.ok());
    REQUIRE(validator.value().append(sample.data, 0x1000).ok());
    REQUIRE(validator.value().append(sample.zero, 0x12000).ok());
    REQUIRE(validator.value().append(sample.discard, 0x13000).ok());
    REQUIRE(validator.value().append(sample.index, 0x14000).ok());
    REQUIRE(validator.value().append(sample.commit_bytes, 0x15000).ok());
    const auto commit = validator.value().finalize();
    REQUIRE(commit.ok());
    REQUIRE_EQ(commit.value(), sample.commit);
}

TEST_CASE(journal_v1_internal_index_node_round_trips) {
    const auto node = sample_internal_node();
    const auto encoded = encode_index_node(node, 0x3000, 0x4000);
    REQUIRE(encoded.ok());
    const auto decoded = decode_index_node(encoded.value(), 0x3000, 0x4000);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value().node, node);
    REQUIRE_EQ(decoded.value().minimum_key, 1ull);
    REQUIRE_EQ(decoded.value().maximum_key, 20ull);
}

TEST_CASE(journal_v1_index_rejects_all_truncations_extra_and_reserved_bytes) {
    const auto sample = independent_sample_checkpoint();
    for (std::size_t size = 0; size < sample.index.size(); ++size) {
        require_index_rejected(
            std::span<const std::byte>{sample.index.data(), size});
    }
    std::vector<std::byte> extra(sample.index.begin(), sample.index.end());
    extra.push_back(std::byte{0});
    require_index_rejected(extra);

    for (const auto offset : {std::size_t{22}, std::size_t{36},
                              std::size_t{104}, std::size_t{127},
                              std::size_t{138}, std::size_t{168},
                              std::size_t{272}, std::size_t{4095}}) {
        auto corrupt = sample.index;
        corrupt[offset] = std::byte{1};
        refresh_record_crc(corrupt);
        require_index_rejected(corrupt);
    }
}

TEST_CASE(journal_v1_index_rejects_unsorted_ranges_and_bad_descriptors) {
    const auto sample = independent_sample_checkpoint();
    for (const auto& mutation :
         std::vector<std::pair<std::size_t, std::uint64_t>>{
             {74, 0}, {74, 83}, {76, 1}, {78, 47}, {80, 2}, {88, 6},
             {96, 4}, {176, 3}, {144, 0x14000}, {152, 4096},
             {160, 0}, {184, 4}}) {
        auto corrupt = sample.index;
        const auto [offset, value] = mutation;
        if (offset == 74 || offset == 76 || offset == 78) {
            put_u16(corrupt, offset, static_cast<std::uint16_t>(value));
        } else {
            put_u64(corrupt, offset, value);
        }
        refresh_record_crc(corrupt);
        require_index_rejected(corrupt);
    }

    auto node = sample_internal_node();
    node.internal_entries[1].minimum_key = 10;
    REQUIRE(!encode_index_node(node, 0x3000, 0x4000).ok());
    node = sample_internal_node();
    node.internal_entries[0].minimum_key = 11;
    REQUIRE(!encode_index_node(node, 0x3000, 0x4000).ok());
    node = sample_internal_node();
    node.internal_entries[0].child_offset = 0x3000;
    REQUIRE(!encode_index_node(node, 0x3000, 0x4000).ok());
}

TEST_CASE(journal_v1_commit_rejects_all_truncations_extra_and_reserved_bytes) {
    const auto sample = independent_sample_checkpoint();
    for (std::size_t size = 0; size < sample.commit_bytes.size(); ++size) {
        require_commit_rejected(
            std::span<const std::byte>{sample.commit_bytes.data(), size});
    }
    std::vector<std::byte> extra(sample.commit_bytes.begin(),
                                 sample.commit_bytes.end());
    extra.push_back(std::byte{0});
    require_commit_rejected(extra);

    for (std::size_t offset = 244; offset < 248; ++offset) {
        auto corrupt = sample.commit_bytes;
        corrupt[offset] = std::byte{1};
        refresh_record_crc(corrupt);
        require_commit_rejected(corrupt);
    }
    for (std::size_t offset = 304; offset < kJournalAlignment; ++offset) {
        auto corrupt = sample.commit_bytes;
        corrupt[offset] = std::byte{1};
        refresh_record_crc(corrupt);
        require_commit_rejected(corrupt);
    }
}

TEST_CASE(journal_v1_commit_rejects_semantic_mutations_with_valid_crc) {
    const auto sample = independent_sample_checkpoint();
    const std::vector<std::pair<std::size_t, std::uint64_t>> mutations{
        {40, 4}, {48, 0}, {56, 3}, {64, 1ULL << 63U},
        {104, 2}, {112, 5}, {120, 6}, {192, UINT64_MAX},
        {200, 2}, {224, 1}, {232, 0}, {248, 0},
        {256, 0x17000}, {296, 3}};
    for (const auto& [offset, value] : mutations) {
        auto corrupt = sample.commit_bytes;
        put_u64(corrupt, offset, value);
        refresh_record_crc(corrupt);
        require_commit_rejected(corrupt);
    }
    for (const auto offset : {std::size_t{72}, std::size_t{88},
                              std::size_t{264}}) {
        auto corrupt = sample.commit_bytes;
        corrupt[offset] ^= std::byte{1};
        refresh_record_crc(corrupt);
        require_commit_rejected(corrupt);
    }
    require_commit_rejected(sample.commit_bytes, 0x15001, 0x16000);
    require_commit_rejected(sample.commit_bytes, 0x15000, 0x17000);
}

TEST_CASE(journal_v1_genesis_commit_round_trips_and_validates_as_sequence) {
    const auto header = sample_header();
    const auto expected = sample_genesis_commit();
    const auto encoded = encode_checkpoint_commit(
        expected, header, kJournalAlignment, 2 * kJournalAlignment);
    REQUIRE(encoded.ok());
    const auto decoded = decode_checkpoint_commit(
        encoded.value(), header, kJournalAlignment,
        2 * kJournalAlignment);
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), expected);

    auto validator = CheckpointSequenceValidator::begin(
        header, 0, 0, header.predecessor_digest, kJournalAlignment,
        2 * kJournalAlignment, 1);
    REQUIRE(validator.ok());
    REQUIRE(validator.value().append(encoded.value(),
                                     kJournalAlignment).ok());
    REQUIRE(validator.value().finalize().ok());

    auto invalid = expected;
    invalid.new_chain_digest[0] ^= std::byte{1};
    REQUIRE(!encode_checkpoint_commit(
                 invalid, header, kJournalAlignment,
                 2 * kJournalAlignment).ok());
}

TEST_CASE(journal_v1_chain_digest_is_sensitive_to_every_canonical_component) {
    const auto sample = independent_sample_checkpoint();
    const auto calculate = [&](CheckpointDigestContext context,
                               const std::vector<std::span<const std::byte>>&
                                   records,
                               std::span<const std::byte> commit_bytes) {
        auto stream = CheckpointChainDigest::begin(context);
        REQUIRE(stream.ok());
        for (const auto record : records) {
            REQUIRE(stream.value().append_non_commit_record(record).ok());
        }
        const auto digest = stream.value().finalize(commit_bytes);
        REQUIRE(digest.ok());
        return digest.value();
    };
    const CheckpointDigestContext original_context{
        sample.commit.previous_digest, sample.commit.image_uuid,
        sample.commit.journal_epoch, sample.commit.previous_generation,
        sample.commit.new_generation, sample.commit.geometry_digest};
    const std::vector<std::span<const std::byte>> original_records{
        sample.data, sample.zero, sample.discard, sample.index};
    REQUIRE_EQ(calculate(original_context, original_records,
                         sample.commit_bytes), sample.digest);

    auto context = original_context;
    context.previous_digest[0] ^= std::byte{1};
    REQUIRE(calculate(context, original_records, sample.commit_bytes) !=
            sample.digest);
    context = original_context;
    context.journal_epoch = 2;
    REQUIRE(calculate(context, original_records, sample.commit_bytes) !=
            sample.digest);

    const std::vector<std::span<const std::byte>> reordered{
        sample.zero, sample.data, sample.discard, sample.index};
    REQUIRE(calculate(original_context, reordered, sample.commit_bytes) !=
            sample.digest);

    auto payload_changed = sample.data;
    payload_changed[kJournalAlignment + 1] ^= std::byte{1};
    put_u32(payload_changed, 112, independent_crc32c(
        std::span<const std::byte>{payload_changed}.subspan(
            kJournalAlignment, kM2BlockSize)));
    refresh_record_crc(payload_changed);
    const std::vector<std::span<const std::byte>> changed_records{
        payload_changed, sample.zero, sample.discard, sample.index};
    REQUIRE(calculate(original_context, changed_records,
                      sample.commit_bytes) != sample.digest);

    auto root_changed = sample.commit_bytes;
    put_u32(root_changed, 240, get_u32(root_changed, 240) ^ 1U);
    refresh_record_crc(root_changed);
    REQUIRE(calculate(original_context, original_records, root_changed) !=
            sample.digest);
}

TEST_CASE(journal_v1_sequence_validator_rejects_gaps_duplicates_and_digest) {
    const auto sample = independent_sample_checkpoint();
    const auto make_validator = [&]() {
        return CheckpointSequenceValidator::begin(
            sample_header(), 4, 5, sample.commit.previous_digest,
            0x1000, 0x16000, 4);
    };
    auto validator = make_validator();
    REQUIRE(validator.ok());
    REQUIRE(!validator.value().append(sample.data, 0x2000).ok());

    validator = make_validator();
    REQUIRE(validator.ok());
    REQUIRE(validator.value().append(sample.data, 0x1000).ok());
    auto duplicate = sample.zero;
    put_u64(duplicate, 72, 3);
    refresh_record_crc(duplicate);
    REQUIRE(!validator.value().append(duplicate, 0x12000).ok());

    validator = make_validator();
    REQUIRE(validator.ok());
    REQUIRE(validator.value().append(sample.data, 0x1000).ok());
    REQUIRE(validator.value().append(sample.zero, 0x12000).ok());
    REQUIRE(validator.value().append(sample.discard, 0x13000).ok());
    REQUIRE(validator.value().append(sample.index, 0x14000).ok());
    auto bad_digest = sample.commit_bytes;
    bad_digest[160] ^= std::byte{1};
    refresh_record_crc(bad_digest);
    REQUIRE(!validator.value().append(bad_digest, 0x15000).ok());
    REQUIRE(!validator.value().finalize().ok());

    validator = make_validator();
    REQUIRE(validator.ok());
    REQUIRE(validator.value().append(sample.data, 0x1000).ok());
    REQUIRE(validator.value().append(sample.zero, 0x12000).ok());
    REQUIRE(validator.value().append(sample.discard, 0x13000).ok());
    REQUIRE(validator.value().append(sample.index, 0x14000).ok());
    auto bad_previous = sample.commit_bytes;
    bad_previous[128] ^= std::byte{1};
    refresh_record_crc(bad_previous);
    REQUIRE(!validator.value().append(bad_previous, 0x15000).ok());

    auto index_first = sample.index;
    put_u64(index_first, 56, 0);
    refresh_record_crc(index_first);
    auto order_validator = CheckpointSequenceValidator::begin(
        sample_header(), 4, 5, sample.commit.previous_digest,
        0x14000, 0x26000, 2);
    REQUIRE(order_validator.ok());
    REQUIRE(order_validator.value().append(index_first, 0x14000).ok());
    auto late_data = sample.data;
    put_u64(late_data, 56, 1);
    refresh_record_crc(late_data);
    REQUIRE(!order_validator.value().append(late_data, 0x15000).ok());
}

TEST_CASE(journal_v1_unknown_type_and_mandatory_flags_fail_closed) {
    const auto sample = independent_sample_checkpoint();
    auto unknown = sample.index;
    put_u16(unknown, 20, 6);
    refresh_record_crc(unknown);
    require_index_rejected(unknown);

    auto mandatory = sample.index;
    put_u64(mandatory, 64, 1ULL << 63U);
    refresh_record_crc(mandatory);
    require_index_rejected(mandatory);
}

TEST_CASE(journal_v1_deterministic_byte_fuzz_rejects_all_record_types) {
    const auto sample = independent_sample_checkpoint();
    const auto geometry = sample_geometry();
    std::uint64_t state = 0xC0FFEE1234567890ULL;
    const auto next_offset = [&](std::size_t size) {
        state = state * 6364136223846793005ULL +
                1442695040888963407ULL;
        return static_cast<std::size_t>(state % size);
    };

    for (const auto* original : {&sample.data, &sample.zero,
                                 &sample.discard}) {
        for (int iteration = 0; iteration < 256; ++iteration) {
            auto corrupt = *original;
            corrupt[next_offset(corrupt.size())] ^= std::byte{0x5A};
            require_block_rejected(corrupt, kJournalAlignment,
                                   kJournalAlignment + corrupt.size(),
                                   geometry);
        }
    }
    for (int iteration = 0; iteration < 256; ++iteration) {
        auto corrupt = sample.index;
        corrupt[next_offset(corrupt.size())] ^= std::byte{0x5A};
        require_index_rejected(corrupt);
    }
    for (int iteration = 0; iteration < 256; ++iteration) {
        auto corrupt = sample.commit_bytes;
        corrupt[next_offset(corrupt.size())] ^= std::byte{0x5A};
        require_commit_rejected(corrupt);
    }
}

}  // namespace
}  // namespace myrd::persistence
