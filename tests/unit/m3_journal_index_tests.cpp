#include "test_framework.h"

#include "src/common/crc32c.h"
#include "src/persistence/journal_index.h"
#include "tests/support/fault_file.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <map>
#include <memory>
#include <stop_token>
#include <thread>
#include <vector>

namespace myrd::persistence {
namespace {

std::uint32_t read_u32(const std::span<const std::byte> bytes,
                       const std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

WindowsFileIdentity sample_file_identity(std::byte marker = std::byte{1}) {
    WindowsFileIdentity identity{};
    identity.volume_serial_number = 7;
    identity.file_id[0] = marker;
    identity.attributes = 1;
    identity.hard_link_count = 1;
    return identity;
}

JournalFileIdentity sample_journal_identity() {
    JournalFileIdentity identity{};
    identity.journal_uuid[0] = std::byte{9};
    return identity;
}

template <typename Buffer>
void put_u16(Buffer& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

template <typename Buffer>
void put_u32(Buffer& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xFFU);
    }
}

template <typename Buffer>
void put_u64(Buffer& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xFFU);
    }
}

template <typename Buffer>
void refresh_crc(Buffer& bytes) {
    put_u32(bytes, 32, 0);
    put_u32(bytes, 32, crc32c(bytes));
}

struct LeafFixture final {
    JournalIndexView view{};
    std::shared_ptr<test::FaultFileState> state{};
};

LeafFixture make_leaf_fixture() {
    JournalIndexNode node{};
    node.common.type = JournalRecordType::IndexNode;
    node.common.checkpoint_generation = 5;
    node.common.operation_id = 17;
    node.level = 0;
    node.leaf_entries = {
        {3, JournalRecordType::Data, 0x11112222, 0x1000,
         kDataRecordBytes, 5},
        {4, JournalRecordType::Zero, 0, 0x12000,
         kFixedRecordBytes, 5},
        {5, JournalRecordType::Discard, 0, 0x13000,
         kFixedRecordBytes, 5}};
    constexpr std::uint64_t node_offset = 0x14000;
    constexpr std::uint64_t committed = 0x16000;
    const auto encoded = encode_index_node(node, node_offset, committed);
    REQUIRE(encoded.ok());

    auto fault = test::make_fault_file(committed);
    REQUIRE(fault.file->write_exact(node_offset, encoded.value()).ok());
    auto shared_file = std::shared_ptr<IPersistenceFile>(
        std::move(fault.file));
    JournalIndexView view{};
    view.file = std::move(shared_file);
    view.file_identity = sample_file_identity();
    view.journal_identity = sample_journal_identity();
    view.epoch = 1;
    view.root_generation = 5;
    view.committed_length = committed;
    view.root = {node_offset, kIndexNodeBytes,
                 read_u32(encoded.value(), 32)};
    return {std::move(view), std::move(fault.state)};
}

struct TreeFixture final {
    JournalIndexView view{};
    std::shared_ptr<test::FaultFileState> state{};
    std::array<std::byte, kIndexNodeBytes> leaf_one{};
    std::array<std::byte, kIndexNodeBytes> leaf_two{};
    std::array<std::byte, kIndexNodeBytes> root{};
    static constexpr std::uint64_t leaf_one_offset = 0x15000;
    static constexpr std::uint64_t leaf_two_offset = 0x16000;
    static constexpr std::uint64_t root_offset = 0x17000;
    static constexpr std::uint64_t committed = 0x18000;
};

TreeFixture make_tree_fixture() {
    JournalIndexNode leaf_one{};
    leaf_one.common = {JournalRecordType::IndexNode, 5, 31, 0, 0};
    leaf_one.level = 0;
    leaf_one.leaf_entries = {
        {0, JournalRecordType::Data, 0x10101010, 0x1000,
         kDataRecordBytes, 5},
        {10, JournalRecordType::Zero, 0, 0x12000,
         kFixedRecordBytes, 5}};
    auto leaf_one_bytes = encode_index_node(
        leaf_one, TreeFixture::leaf_one_offset, TreeFixture::committed);
    REQUIRE(leaf_one_bytes.ok());

    JournalIndexNode leaf_two{};
    leaf_two.common = {JournalRecordType::IndexNode, 5, 31, 1, 0};
    leaf_two.level = 0;
    leaf_two.leaf_entries = {
        {20, JournalRecordType::Discard, 0, 0x13000,
         kFixedRecordBytes, 5},
        {30, JournalRecordType::Zero, 0, 0x14000,
         kFixedRecordBytes, 5}};
    auto leaf_two_bytes = encode_index_node(
        leaf_two, TreeFixture::leaf_two_offset, TreeFixture::committed);
    REQUIRE(leaf_two_bytes.ok());

    JournalIndexNode root{};
    root.common = {JournalRecordType::IndexNode, 5, 31, 2, 0};
    root.level = 1;
    root.internal_entries = {
        {0, 10, TreeFixture::leaf_one_offset, kIndexNodeBytes,
         read_u32(leaf_one_bytes.value(), 32), 5},
        {20, 30, TreeFixture::leaf_two_offset, kIndexNodeBytes,
         read_u32(leaf_two_bytes.value(), 32), 5}};
    auto root_bytes = encode_index_node(
        root, TreeFixture::root_offset, TreeFixture::committed);
    REQUIRE(root_bytes.ok());

    auto fault = test::make_fault_file(TreeFixture::committed);
    REQUIRE(fault.file->write_exact(
        TreeFixture::leaf_one_offset, leaf_one_bytes.value()).ok());
    REQUIRE(fault.file->write_exact(
        TreeFixture::leaf_two_offset, leaf_two_bytes.value()).ok());
    REQUIRE(fault.file->write_exact(
        TreeFixture::root_offset, root_bytes.value()).ok());
    JournalIndexView view{};
    view.file = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    view.file_identity = sample_file_identity();
    view.journal_identity = sample_journal_identity();
    view.epoch = 1;
    view.root_generation = 5;
    view.committed_length = TreeFixture::committed;
    view.root = {TreeFixture::root_offset, kIndexNodeBytes,
                 read_u32(root_bytes.value(), 32)};
    return {std::move(view), std::move(fault.state),
            leaf_one_bytes.value(), leaf_two_bytes.value(),
            root_bytes.value()};
}

void require_journal_corrupt(const Result<JournalIndexLookupResult>& result) {
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::persistence);
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::JournalCorrupt));
}

TEST_CASE(journal_index_leaf_resolves_data_zero_discard_and_absent) {
    auto fixture = make_leaf_fixture();
    auto reader_result = JournalIndexReader::create({4 * kIndexNodeBytes, 4});
    REQUIRE(reader_result.ok());
    auto& reader = *reader_result.value();

    const auto data = reader.lookup(fixture.view, 3);
    REQUIRE(data.ok());
    REQUIRE_EQ(data.value().kind, JournalIndexLookupKind::Data);
    REQUIRE_EQ(data.value().descriptor.block_index, 3ull);

    const auto zero = reader.lookup(fixture.view, 4);
    REQUIRE(zero.ok());
    REQUIRE_EQ(zero.value().kind, JournalIndexLookupKind::Zero);

    const auto discard = reader.lookup(fixture.view, 5);
    REQUIRE(discard.ok());
    REQUIRE_EQ(discard.value().kind, JournalIndexLookupKind::Discard);

    const auto absent = reader.lookup(fixture.view, 6);
    REQUIRE(absent.ok());
    REQUIRE_EQ(absent.value().kind, JournalIndexLookupKind::Absent);
}

TEST_CASE(journal_index_empty_root_is_absent_without_io) {
    auto fixture = make_leaf_fixture();
    fixture.view.root = {};
    fixture.view.root_generation = 0;
    auto reader = JournalIndexReader::create({kIndexNodeBytes, 1});
    REQUIRE(reader.ok());
    const auto before = fixture.state->calls(test::FaultFilePhase::Read);
    const auto result = reader.value()->lookup(fixture.view, 0);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().kind, JournalIndexLookupKind::Absent);
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Read), before);
}

TEST_CASE(journal_index_multilevel_lookup_checks_boundaries_and_gaps) {
    auto fixture = make_tree_fixture();
    auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
    REQUIRE(reader.ok());
    for (const auto& [key, kind] :
         std::vector<std::pair<std::uint64_t, JournalIndexLookupKind>>{
             {0, JournalIndexLookupKind::Data},
             {10, JournalIndexLookupKind::Zero},
             {20, JournalIndexLookupKind::Discard},
             {30, JournalIndexLookupKind::Zero},
             {11, JournalIndexLookupKind::Absent},
             {19, JournalIndexLookupKind::Absent},
             {31, JournalIndexLookupKind::Absent},
             {UINT64_MAX, JournalIndexLookupKind::Absent}}) {
        const auto result = reader.value()->lookup(fixture.view, key);
        REQUIRE(result.ok());
        REQUIRE_EQ(result.value().kind, kind);
    }
}

TEST_CASE(journal_index_accepts_exactly_eight_levels) {
    constexpr std::uint64_t leaf_offset = 0x12000;
    constexpr std::uint64_t root_offset = 0x19000;
    constexpr std::uint64_t committed = 0x1A000;
    auto fault = test::make_fault_file(committed);

    JournalIndexNode leaf{};
    leaf.common = {JournalRecordType::IndexNode, 8, 55, 0, 0};
    leaf.level = 0;
    leaf.leaf_entries = {{42, JournalRecordType::Data, 0x12345678,
                          0x1000, kDataRecordBytes, 8}};
    auto child = encode_index_node(leaf, leaf_offset, committed);
    REQUIRE(child.ok());
    REQUIRE(fault.file->write_exact(leaf_offset, child.value()).ok());
    auto child_offset = leaf_offset;
    auto child_crc = read_u32(child.value(), 32);
    for (std::uint16_t level = 1; level < kMaxIndexDepth; ++level) {
        const auto offset = leaf_offset +
                            static_cast<std::uint64_t>(level) *
                                kIndexNodeBytes;
        JournalIndexNode parent{};
        parent.common = {JournalRecordType::IndexNode, 8, 55, level, 0};
        parent.level = level;
        parent.internal_entries = {
            {42, 42, child_offset, kIndexNodeBytes, child_crc, 8}};
        child = encode_index_node(parent, offset, committed);
        REQUIRE(child.ok());
        REQUIRE(fault.file->write_exact(offset, child.value()).ok());
        child_offset = offset;
        child_crc = read_u32(child.value(), 32);
    }
    REQUIRE_EQ(child_offset, root_offset);

    JournalIndexView view{};
    view.file = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    view.file_identity = sample_file_identity();
    view.journal_identity = sample_journal_identity();
    view.epoch = 1;
    view.root_generation = 8;
    view.committed_length = committed;
    view.root = {root_offset, kIndexNodeBytes, child_crc};
    auto reader = JournalIndexReader::create({8 * kIndexNodeBytes, 8});
    REQUIRE(reader.ok());
    test::reset_journal_index_guard_test_counters();
    const auto result = reader.value()->lookup(view, 42);
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().kind, JournalIndexLookupKind::Data);
    REQUIRE_EQ(reader.value()->cache_stats().read_calls, 8ull);
    const auto guards = test::journal_index_guard_test_counters();
    REQUIRE(guards.visited_calls >= 8);
    REQUIRE(guards.depth_calls >= 7);
}

TEST_CASE(journal_index_corruption_never_becomes_absent) {
    {
        auto fixture = make_tree_fixture();
        fixture.root[500] ^= std::byte{1};
        REQUIRE(fixture.view.file->write_exact(
            TreeFixture::root_offset, fixture.root).ok());
        auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
        REQUIRE(reader.ok());
        require_journal_corrupt(reader.value()->lookup(fixture.view, 0));
    }
    {
        auto fixture = make_tree_fixture();
        put_u64(fixture.leaf_one, 176, 0);
        refresh_crc(fixture.leaf_one);
        REQUIRE(fixture.view.file->write_exact(
            TreeFixture::leaf_one_offset, fixture.leaf_one).ok());
        auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
        REQUIRE(reader.ok());
        require_journal_corrupt(reader.value()->lookup(fixture.view, 10));
    }
    {
        auto fixture = make_tree_fixture();
        put_u64(fixture.root, 144, TreeFixture::root_offset);
        refresh_crc(fixture.root);
        fixture.view.root.crc32c = read_u32(fixture.root, 32);
        REQUIRE(fixture.view.file->write_exact(
            TreeFixture::root_offset, fixture.root).ok());
        auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
        REQUIRE(reader.ok());
        require_journal_corrupt(reader.value()->lookup(fixture.view, 0));
    }
    {
        auto fixture = make_tree_fixture();
        put_u64(fixture.root, 144, TreeFixture::committed);
        refresh_crc(fixture.root);
        fixture.view.root.crc32c = read_u32(fixture.root, 32);
        REQUIRE(fixture.view.file->write_exact(
            TreeFixture::root_offset, fixture.root).ok());
        auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
        REQUIRE(reader.ok());
        require_journal_corrupt(reader.value()->lookup(fixture.view, 0));
    }
    {
        auto fixture = make_tree_fixture();
        put_u16(fixture.root, 72, 8);
        refresh_crc(fixture.root);
        fixture.view.root.crc32c = read_u32(fixture.root, 32);
        REQUIRE(fixture.view.file->write_exact(
            TreeFixture::root_offset, fixture.root).ok());
        auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
        REQUIRE(reader.ok());
        require_journal_corrupt(reader.value()->lookup(fixture.view, 0));
    }
}

TEST_CASE(journal_index_revalidates_child_crc_level_and_range) {
    {
        auto fixture = make_tree_fixture();
        put_u32(fixture.root, 160, 0xDEADBEEF);
        refresh_crc(fixture.root);
        fixture.view.root.crc32c = read_u32(fixture.root, 32);
        REQUIRE(fixture.view.file->write_exact(
            TreeFixture::root_offset, fixture.root).ok());
        auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
        REQUIRE(reader.ok());
        require_journal_corrupt(reader.value()->lookup(fixture.view, 0));
    }
    {
        auto fixture = make_tree_fixture();
        put_u16(fixture.root, 72, 2);
        refresh_crc(fixture.root);
        fixture.view.root.crc32c = read_u32(fixture.root, 32);
        REQUIRE(fixture.view.file->write_exact(
            TreeFixture::root_offset, fixture.root).ok());
        auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
        REQUIRE(reader.ok());
        require_journal_corrupt(reader.value()->lookup(fixture.view, 0));
    }
    {
        auto fixture = make_tree_fixture();
        put_u64(fixture.root, 80, 1);
        put_u64(fixture.root, 128, 1);
        refresh_crc(fixture.root);
        fixture.view.root.crc32c = read_u32(fixture.root, 32);
        REQUIRE(fixture.view.file->write_exact(
            TreeFixture::root_offset, fixture.root).ok());
        auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
        REQUIRE(reader.ok());
        require_journal_corrupt(reader.value()->lookup(fixture.view, 5));
    }
}

TEST_CASE(journal_index_cache_is_bounded_lru_and_reuses_nodes) {
    auto fixture = make_tree_fixture();
    auto reader = JournalIndexReader::create({2 * kIndexNodeBytes, 2});
    REQUIRE(reader.ok());
    REQUIRE(reader.value()->lookup(fixture.view, 0).ok());
    REQUIRE_EQ(reader.value()->cache_stats().read_calls, 2ull);
    REQUIRE(reader.value()->lookup(fixture.view, 10).ok());
    REQUIRE_EQ(reader.value()->cache_stats().read_calls, 2ull);
    REQUIRE(reader.value()->lookup(fixture.view, 20).ok());
    REQUIRE(reader.value()->lookup(fixture.view, 0).ok());
    const auto stats = reader.value()->cache_stats();
    REQUIRE(stats.hits >= 3);
    REQUIRE(stats.evictions >= 2);
    REQUIRE(stats.current_entries <= 2);
    REQUIRE(stats.current_bytes <= 2 * kIndexNodeBytes);
    REQUIRE(stats.peak_entries <= 2);
    REQUIRE(stats.peak_bytes <= 2 * kIndexNodeBytes);

    reader.value()->clear_cache();
    const auto cleared = reader.value()->cache_stats();
    REQUIRE_EQ(cleared.current_entries, 0ull);
    REQUIRE_EQ(cleared.current_bytes, 0ull);
}

TEST_CASE(journal_index_cache_identity_prevents_cross_view_reuse) {
    auto first = make_leaf_fixture();
    auto second = make_leaf_fixture();
    second.view.file_identity = sample_file_identity(std::byte{2});
    auto reader = JournalIndexReader::create({8 * kIndexNodeBytes, 8});
    REQUIRE(reader.ok());
    REQUIRE(reader.value()->lookup(first.view, 3).ok());
    REQUIRE(reader.value()->lookup(second.view, 3).ok());
    REQUIRE_EQ(first.state->calls(test::FaultFilePhase::Read), 1ull);
    REQUIRE_EQ(second.state->calls(test::FaultFilePhase::Read), 1ull);

    auto epoch_view = first.view;
    epoch_view.epoch = 2;
    REQUIRE(reader.value()->lookup(epoch_view, 3).ok());
    auto journal_view = first.view;
    journal_view.journal_identity.journal_uuid[1] = std::byte{1};
    REQUIRE(reader.value()->lookup(journal_view, 3).ok());
    auto offset_view = first.view;
    offset_view.root.offset = 0x15000;
    // The same immutable node is copied to another aligned offset so only
    // the node/root offset identity changes.
    auto source = make_leaf_fixture();
    std::array<std::byte, kIndexNodeBytes> node_bytes{};
    REQUIRE(source.view.file->read_exact(0x14000, node_bytes).ok());
    REQUIRE(first.view.file->write_exact(0x15000, node_bytes).ok());
    offset_view.root.crc32c = read_u32(node_bytes, 32);
    REQUIRE(reader.value()->lookup(offset_view, 3).ok());

    auto length_view = first.view;
    length_view.committed_length += kJournalAlignment;
    REQUIRE(reader.value()->lookup(length_view, 3).ok());
    REQUIRE_EQ(first.state->calls(test::FaultFilePhase::Read), 5ull);
}

TEST_CASE(journal_index_rejects_invalid_views_limits_and_cancelled_reads) {
    REQUIRE(!JournalIndexReader::create({0, 1}).ok());
    REQUIRE(!JournalIndexReader::create({kIndexNodeBytes, 0}).ok());
    REQUIRE(!JournalIndexReader::create({kIndexNodeBytes, 2}).ok());

    auto fixture = make_leaf_fixture();
    auto reader = JournalIndexReader::create({kIndexNodeBytes, 1});
    REQUIRE(reader.ok());
    auto invalid = fixture.view;
    invalid.root.offset = invalid.committed_length;
    require_journal_corrupt(reader.value()->lookup(invalid, 3));
    invalid = fixture.view;
    invalid.root.length = 0;
    require_journal_corrupt(reader.value()->lookup(invalid, 3));
    invalid = fixture.view;
    invalid.file_identity.file_id = {};
    require_journal_corrupt(reader.value()->lookup(invalid, 3));
    invalid = fixture.view;
    invalid.root.offset = UINT64_MAX - kJournalAlignment + 1;
    invalid.committed_length = UINT64_MAX - kJournalAlignment + 1;
    require_journal_corrupt(reader.value()->lookup(invalid, 3));

    std::stop_source stop;
    stop.request_stop();
    OperationContext context{stop.get_token(),
                             std::chrono::steady_clock::time_point::max()};
    const auto before = fixture.state->calls(test::FaultFilePhase::Read);
    REQUIRE(!reader.value()->lookup(context, fixture.view, 3).ok());
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Read), before);
}

TEST_CASE(journal_index_traversal_guards_have_direct_mutation_proof) {
    const std::array<std::uint64_t, 3> visited{0x1000, 0x2000, 0x3000};
    REQUIRE(!test::journal_index_test_rejects_visited_offset(
                  visited, 0x2000).ok());
    REQUIRE(test::journal_index_test_rejects_visited_offset(
                visited, 0x4000).ok());
    REQUIRE(test::journal_index_test_rejects_depth_overflow(7, 1).error().code
            == static_cast<std::uint32_t>(
                PersistenceErrorCode::JournalCorrupt));
    REQUIRE(test::journal_index_test_rejects_depth_overflow(6, 1).ok());
    REQUIRE(test::journal_index_test_rejects_depth_overflow(7, 0).ok());
}

TEST_CASE(journal_index_cache_is_safe_under_concurrent_hits_and_eviction) {
    auto fixture = make_tree_fixture();
    auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
    REQUIRE(reader.ok());
    std::atomic<bool> failed{false};
    std::vector<std::thread> workers;
    for (int index = 0; index < 8; ++index) {
        workers.emplace_back([&] {
            for (int iteration = 0; iteration < 200; ++iteration) {
                const auto key = static_cast<std::uint64_t>(iteration % 4) *
                                 10;
                const auto result = reader.value()->lookup(fixture.view, key);
                if (!result.ok()) failed.store(true);
            }
        });
    }
    for (auto& worker : workers) worker.join();
    REQUIRE(!failed.load());
    const auto stats = reader.value()->cache_stats();
    REQUIRE(stats.current_entries <= 3);
    REQUIRE(stats.current_bytes <= 3 * kIndexNodeBytes);
    REQUIRE(stats.peak_entries <= 3);
    REQUIRE(stats.peak_bytes <= 3 * kIndexNodeBytes);
}

TEST_CASE(journal_index_deadline_expires_while_cache_mutex_is_held) {
    auto fixture = make_leaf_fixture();
    auto reader = JournalIndexReader::create({kIndexNodeBytes, 1});
    REQUIRE(reader.ok());
    REQUIRE(reader.value()->lookup(fixture.view, 3).ok());

    std::atomic<bool> entered{false};
    std::atomic<bool> holder_ok{false};
    std::thread holder([&] {
        holder_ok.store(reader.value()->hold_cache_lock_for_test(
                            entered, std::chrono::milliseconds{100}).ok(),
                        std::memory_order_release);
    });
    while (!entered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    OperationContext context{
        {}, std::chrono::steady_clock::now() +
                 std::chrono::milliseconds{10}};
    const auto result = reader.value()->lookup(context, fixture.view, 3);
    holder.join();
    REQUIRE(holder_ok.load(std::memory_order_acquire));
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().domain, ErrorDomain::storage);
    REQUIRE_EQ(result.error().code,
               static_cast<std::uint32_t>(OperationErrorCode::timed_out));
}

TEST_CASE(journal_index_one_million_random_lookups_match_map_oracle) {
    auto fixture = make_tree_fixture();
    auto reader = JournalIndexReader::create({3 * kIndexNodeBytes, 3});
    REQUIRE(reader.ok());
    const std::map<std::uint64_t, JournalIndexLookupKind> oracle{
        {0, JournalIndexLookupKind::Data},
        {10, JournalIndexLookupKind::Zero},
        {20, JournalIndexLookupKind::Discard},
        {30, JournalIndexLookupKind::Zero}};
    std::uint64_t state = 0x17A5B4C3D2E1908FULL;
    for (std::uint64_t iteration = 0; iteration < 1'000'000; ++iteration) {
        state = state * 6364136223846793005ULL +
                1442695040888963407ULL;
        const auto key = (state >> 32U) % 64U;
        const auto result = reader.value()->lookup(fixture.view, key);
        REQUIRE(result.ok());
        const auto found = oracle.find(key);
        const auto expected = found == oracle.end()
            ? JournalIndexLookupKind::Absent
            : found->second;
        REQUIRE_EQ(result.value().kind, expected);
    }
    const auto stats = reader.value()->cache_stats();
    REQUIRE_EQ(stats.read_calls, 3ull);
    REQUIRE(stats.peak_bytes <= 3 * kIndexNodeBytes);
}

}  // namespace
}  // namespace myrd::persistence
