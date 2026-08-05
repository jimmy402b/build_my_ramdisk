#include "test_framework.h"

#include "src/persistence/journal_index.h"
#include "tests/support/fault_file.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stop_token>
#include <vector>

namespace myrd::persistence {
namespace {

WindowsFileIdentity file_identity() {
    WindowsFileIdentity identity{};
    identity.volume_serial_number = 19;
    identity.file_id[0] = std::byte{0x31};
    identity.attributes = 1;
    identity.hard_link_count = 1;
    return identity;
}

JournalFileIdentity journal_identity() {
    JournalFileIdentity identity{};
    identity.journal_uuid[0] = std::byte{0x42};
    return identity;
}

struct BuilderFixture final {
    JournalIndexView view{};
    std::shared_ptr<test::FaultFileState> state{};
};

BuilderFixture empty_fixture(const std::uint64_t logical_length) {
    auto fault = test::make_fault_file(logical_length);
    JournalIndexView view{};
    view.file = std::shared_ptr<IPersistenceFile>(std::move(fault.file));
    view.file_identity = file_identity();
    view.journal_identity = journal_identity();
    view.epoch = 1;
    view.root_generation = 0;
    view.committed_length = kJournalAlignment;
    return {std::move(view), std::move(fault.state)};
}

std::unique_ptr<JournalIndexBuilder> make_builder(
    const std::uint64_t max_nodes = 4096) {
    auto builder = JournalIndexBuilder::create(
        {{64 * kIndexNodeBytes, 64}, max_nodes,
         max_nodes * kIndexNodeBytes});
    REQUIRE(builder.ok());
    return builder.move_value();
}

JournalIndexLeafEntry zero_update(const std::uint64_t key,
                                  const CheckpointGeneration generation,
                                  const std::uint64_t offset) {
    return {key, JournalRecordType::Zero, 0, offset,
            kFixedRecordBytes, generation};
}

JournalIndexLeafEntry typed_update(const std::uint64_t key,
                                   const CheckpointGeneration generation,
                                   const std::uint64_t offset,
                                   const JournalRecordType type) {
    return {key, type,
            type == JournalRecordType::Data ? 0xC001D00Du : 0u,
            offset,
            type == JournalRecordType::Data
                ? kDataRecordBytes : kFixedRecordBytes,
            generation};
}

JournalIndexLookupKind lookup_kind(const JournalRecordType type) {
    switch (type) {
    case JournalRecordType::Data: return JournalIndexLookupKind::Data;
    case JournalRecordType::Zero: return JournalIndexLookupKind::Zero;
    case JournalRecordType::Discard: return JournalIndexLookupKind::Discard;
    default: return JournalIndexLookupKind::Absent;
    }
}

void write_assigned(const JournalIndexView& view,
                    const JournalIndexAssignedPlan& assigned) {
    for (const auto& node : assigned.nodes) {
        REQUIRE(view.file->write_exact(node.offset, node.bytes).ok());
    }
}

JournalIndexView committed_view(const JournalIndexView& old,
                                const JournalIndexAssignedPlan& assigned,
                                const CheckpointGeneration generation,
                                const std::uint64_t committed_length) {
    auto view = old;
    view.root_generation = generation;
    view.committed_length = committed_length;
    view.root = assigned.root;
    return view;
}

TEST_CASE(journal_index_builder_is_pure_deterministic_and_rejects_duplicates) {
    auto fixture = empty_fixture(0x20000);
    auto builder = make_builder();
    const std::vector<JournalIndexLeafEntry> unordered{
        zero_update(9, 1, 0x12000),
        {3, JournalRecordType::Data, 0x12345678, 0x1000,
         kDataRecordBytes, 1},
        zero_update(5, 1, 0x13000)};
    const auto writes_before = fixture.state->calls(test::FaultFilePhase::Write);
    auto first = builder->plan(OperationContext{}, fixture.view, unordered, 1, 7);
    REQUIRE(first.ok());
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write), writes_before);

    auto ordered = unordered;
    std::sort(ordered.begin(), ordered.end(), [](const auto& left,
                                                  const auto& right) {
        return left.block_index < right.block_index;
    });
    auto second = builder->plan(OperationContext{}, fixture.view, ordered, 1, 7);
    REQUIRE(second.ok());
    REQUIRE_EQ(first.value(), second.value());

    auto assigned_one = builder->assign_offsets(
        first.value(), fixture.view, 0x14000, 10, 0x16000);
    auto assigned_two = builder->assign_offsets(
        second.value(), fixture.view, 0x14000, 10, 0x16000);
    REQUIRE(assigned_one.ok());
    REQUIRE(assigned_two.ok());
    REQUIRE_EQ(assigned_one.value().root, assigned_two.value().root);
    REQUIRE_EQ(assigned_one.value().nodes.size(), 1ull);
    REQUIRE_EQ(assigned_one.value().nodes[0].bytes,
               assigned_two.value().nodes[0].bytes);

    auto duplicate = unordered;
    duplicate.push_back(zero_update(5, 1, 0x15000));
    auto rejected = builder->plan(OperationContext{}, fixture.view, duplicate,
                                  1, 7);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write), writes_before);

    write_assigned(fixture.view, assigned_one.value());
    auto view = committed_view(fixture.view, assigned_one.value(), 1, 0x16000);
    auto reader = JournalIndexReader::create({4 * kIndexNodeBytes, 4});
    REQUIRE(reader.ok());
    REQUIRE_EQ(reader.value()->lookup(view, 3).value().kind,
               JournalIndexLookupKind::Data);
    REQUIRE_EQ(reader.value()->lookup(view, 5).value().kind,
               JournalIndexLookupKind::Zero);
    REQUIRE_EQ(reader.value()->lookup(view, 4).value().kind,
               JournalIndexLookupKind::Absent);

    const std::vector<JournalIndexLeafEntry> replacements{
        zero_update(3, 2, 0x16000),
        {5, JournalRecordType::Data, 0xAABBCCDD, 0x17000,
         kDataRecordBytes, 2}};
    auto replacement_plan = builder->plan(OperationContext{}, view,
                                           replacements, 2, 8);
    REQUIRE(replacement_plan.ok());
    auto replacement_nodes = builder->assign_offsets(
        replacement_plan.value(), view, 0x28000, 20, 0x29000);
    REQUIRE(replacement_nodes.ok());
    write_assigned(view, replacement_nodes.value());
    auto replacement_view = committed_view(
        view, replacement_nodes.value(), 2, 0x29000);
    REQUIRE_EQ(reader.value()->lookup(replacement_view, 3).value().kind,
               JournalIndexLookupKind::Zero);
    REQUIRE_EQ(reader.value()->lookup(replacement_view, 5).value().kind,
               JournalIndexLookupKind::Data);
}

TEST_CASE(journal_index_builder_splits_and_reuses_unchanged_subtree) {
    constexpr std::uint64_t update_count = kJournalIndexMaxEntries + 1;
    constexpr std::uint64_t first_node =
        kJournalAlignment * (update_count + 1);
    constexpr std::uint64_t old_committed = first_node + 3 * kIndexNodeBytes;
    constexpr std::uint64_t final_committed = old_committed +
        kFixedRecordBytes + 2 * kIndexNodeBytes;
    auto fixture = empty_fixture(final_committed);
    auto builder = make_builder();
    std::vector<JournalIndexLeafEntry> updates;
    for (std::uint64_t key = 0; key < update_count; ++key) {
        updates.push_back(zero_update(
            key, 1, kJournalAlignment * (key + 1)));
    }
    auto plan = builder->plan(OperationContext{}, fixture.view, updates, 1, 11);
    REQUIRE(plan.ok());
    REQUIRE_EQ(plan.value().planned_nodes().size(), 3ull);
    REQUIRE_EQ(plan.value().root_reference().level, 1u);
    auto assigned = builder->assign_offsets(
        plan.value(), fixture.view, first_node, 1, old_committed);
    REQUIRE(assigned.ok());
    write_assigned(fixture.view, assigned.value());
    auto old_view = committed_view(fixture.view, assigned.value(), 1,
                                   old_committed);

    const auto replacement_offset = old_committed;
    const std::vector<JournalIndexLeafEntry> replacement{
        {0, JournalRecordType::Discard, 0, replacement_offset,
         kFixedRecordBytes, 2}};
    const auto writes_before_plan =
        fixture.state->calls(test::FaultFilePhase::Write);
    auto changed = builder->plan(OperationContext{}, old_view, replacement,
                                 2, 12);
    REQUIRE(changed.ok());
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write),
               writes_before_plan);
    REQUIRE_EQ(changed.value().planned_nodes().size(), 2ull);
    const auto& new_root = changed.value().planned_nodes().back();
    REQUIRE_EQ(new_root.level, 1u);
    REQUIRE_EQ(new_root.children.size(), 2ull);
    REQUIRE(new_root.children[0].planned);
    REQUIRE(!new_root.children[1].planned);
    REQUIRE_EQ(new_root.children[1].offset, assigned.value().nodes[1].offset);

    auto next = builder->assign_offsets(
        changed.value(), old_view,
        replacement_offset + kFixedRecordBytes, 20, final_committed);
    REQUIRE(next.ok());
    write_assigned(old_view, next.value());
    auto view = committed_view(old_view, next.value(), 2, final_committed);
    auto reader = JournalIndexReader::create({8 * kIndexNodeBytes, 8});
    REQUIRE(reader.ok());
    REQUIRE_EQ(reader.value()->lookup(view, 0).value().kind,
               JournalIndexLookupKind::Discard);
    REQUIRE_EQ(reader.value()->lookup(view, update_count - 1).value().kind,
               JournalIndexLookupKind::Zero);
    REQUIRE_EQ(reader.value()->lookup(old_view, 0).value().kind,
               JournalIndexLookupKind::Zero);
}

TEST_CASE(journal_index_builder_fails_closed_on_limits_context_and_bad_plan) {
    auto fixture = empty_fixture(0x100000);
    auto builder = make_builder();
    REQUIRE(!JournalIndexBuilder::create(
        {{kIndexNodeBytes, 1}, 2, kIndexNodeBytes}).ok());
    const std::vector<JournalIndexLeafEntry> update{
        zero_update(1, 1, 0x1000)};
    REQUIRE(!builder->plan(OperationContext{}, fixture.view, {}, 1, 1).ok());
    REQUIRE(!builder->plan(OperationContext{}, fixture.view, update, 0, 1).ok());
    auto skipped = update;
    skipped[0].record_generation = 2;
    REQUIRE(!builder->plan(OperationContext{}, fixture.view, skipped, 2, 1).ok());

    auto exhausted_view = fixture.view;
    exhausted_view.root_generation = kInvalidGeneration - 1;
    auto exhausted_update = update;
    exhausted_update[0].record_generation = kInvalidGeneration - 1;
    REQUIRE(!builder->plan(OperationContext{}, exhausted_view,
                           exhausted_update, kInvalidGeneration - 1, 1).ok());

    auto invalid = update;
    invalid[0].payload_crc32c = 1;
    REQUIRE(!builder->plan(OperationContext{}, fixture.view, invalid, 1, 1).ok());
    const std::vector<JournalIndexLeafEntry> overlap{
        zero_update(1, 1, 0x1000), zero_update(2, 1, 0x1000)};
    REQUIRE(!builder->plan(OperationContext{}, fixture.view, overlap, 1, 1).ok());

    std::stop_source stopped;
    stopped.request_stop();
    REQUIRE(!builder->plan(OperationContext{stopped.get_token()}, fixture.view,
                           update, 1, 1).ok());

    auto plan = builder->plan(OperationContext{}, fixture.view, update, 1, 1);
    REQUIRE(plan.ok());
    REQUIRE(!builder->assign_offsets(
        plan.value(), fixture.view, 0x2001, 1, 0x4000).ok());
    REQUIRE(!builder->assign_offsets(
        plan.value(), fixture.view, 0x2000, 1, 0x2000).ok());
    REQUIRE(!builder->assign_offsets(
        plan.value(), fixture.view, 0x1000, 1, 0x3000).ok());
    REQUIRE(builder->assign_offsets(
        plan.value(), fixture.view, 0x2000,
        (std::numeric_limits<std::uint64_t>::max)(), 0x3000).ok());

    auto wrong_base = fixture.view;
    wrong_base.file_identity.file_id[0] = std::byte{0x7F};
    REQUIRE(!builder->assign_offsets(
        plan.value(), wrong_base, 0x2000, 1, 0x3000).ok());

    auto tiny = JournalIndexBuilder::create(
        {{kIndexNodeBytes, 1}, 1, kIndexNodeBytes});
    REQUIRE(tiny.ok());
    std::vector<JournalIndexLeafEntry> split_updates;
    for (std::uint64_t key = 0; key <= kJournalIndexMaxEntries; ++key) {
        split_updates.push_back(zero_update(
            key, 1, kJournalAlignment * (key + 1)));
    }
    const auto writes_before = fixture.state->calls(test::FaultFilePhase::Write);
    REQUIRE(!tiny.value()->plan(OperationContext{}, fixture.view,
                                split_updates, 1, 2).ok());
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write), writes_before);
}

TEST_CASE(journal_index_builder_splits_internal_root_and_reuses_multilevel_path) {
    constexpr std::uint64_t entry_count =
        static_cast<std::uint64_t>(kJournalIndexMaxEntries) *
            kJournalIndexMaxEntries + 1;
    constexpr std::uint64_t leaf_count =
        (entry_count + kJournalIndexMaxEntries - 1) /
        kJournalIndexMaxEntries;
    constexpr std::uint64_t level_one_count =
        (leaf_count + kJournalIndexMaxEntries - 1) /
        kJournalIndexMaxEntries;
    constexpr std::uint64_t node_count = leaf_count + level_one_count + 1;
    constexpr std::uint64_t first_node =
        kJournalAlignment * (entry_count + 1);
    constexpr std::uint64_t old_committed =
        first_node + node_count * kIndexNodeBytes;
    constexpr std::uint64_t final_committed =
        old_committed + kFixedRecordBytes + 3 * kIndexNodeBytes;
    auto fixture = empty_fixture(final_committed);
    auto builder = make_builder();
    std::vector<JournalIndexLeafEntry> updates;
    updates.reserve(entry_count);
    for (std::uint64_t key = 0; key < entry_count; ++key) {
        updates.push_back(zero_update(
            key, 1, kJournalAlignment * (key + 1)));
    }
    auto plan = builder->plan(OperationContext{}, fixture.view, updates, 1, 40);
    REQUIRE(plan.ok());
    REQUIRE_EQ(plan.value().planned_nodes().size(), node_count);
    REQUIRE_EQ(plan.value().root_reference().level, 2u);
    std::uint64_t level_one_nodes = 0;
    for (const auto& node : plan.value().planned_nodes()) {
        if (node.level == 1) ++level_one_nodes;
    }
    REQUIRE_EQ(level_one_nodes, 2ull);
    auto assigned = builder->assign_offsets(
        plan.value(), fixture.view, first_node, 1, old_committed);
    REQUIRE(assigned.ok());
    write_assigned(fixture.view, assigned.value());
    auto old_view = committed_view(
        fixture.view, assigned.value(), 1, old_committed);

    const std::vector<JournalIndexLeafEntry> replacement{
        zero_update(0, 2, old_committed)};
    const std::vector<JournalIndexLeafEntry> stale_range{
        zero_update(0, 2, old_committed - kFixedRecordBytes)};
    REQUIRE(!builder->plan(OperationContext{}, old_view, stale_range,
                           2, 41).ok());
    const auto writes_before = fixture.state->calls(test::FaultFilePhase::Write);
    auto changed = builder->plan(OperationContext{}, old_view, replacement,
                                 2, 41);
    REQUIRE(changed.ok());
    REQUIRE_EQ(fixture.state->calls(test::FaultFilePhase::Write), writes_before);
    REQUIRE_EQ(changed.value().planned_nodes().size(), 3ull);
    const auto& root = changed.value().planned_nodes().back();
    REQUIRE_EQ(root.level, 2u);
    REQUIRE_EQ(root.children.size(), 2ull);
    REQUIRE(root.children[0].planned);
    REQUIRE(!root.children[1].planned);
    REQUIRE_EQ(root.children[1].offset,
               assigned.value().nodes[leaf_count + 1].offset);
}

TEST_CASE(journal_index_builder_100000_updates_match_map_oracle) {
    constexpr std::uint64_t update_count = 100000;
    constexpr std::uint64_t batch_size = 1000;
    auto fixture = empty_fixture(16ull << 30U);
    auto builder = make_builder();
    std::map<std::uint64_t, JournalIndexLookupKind> oracle;
    std::vector<std::uint64_t> inserted_keys;
    inserted_keys.reserve(update_count);
    auto reader = JournalIndexReader::create({256 * kIndexNodeBytes, 256});
    REQUIRE(reader.ok());
    auto view = fixture.view;
    std::uint64_t append_offset = kJournalAlignment;
    std::uint64_t sequence = 100;
#if !defined(NDEBUG)
    std::uint64_t random_state = 0x18C0FFEE51A7B11DULL;
#endif
    std::uint64_t new_ordinal = 0;
    std::uint64_t event_count = 0;
    constexpr JournalRecordType kinds[]{
        JournalRecordType::Data,
        JournalRecordType::Zero,
        JournalRecordType::Discard};
    for (std::uint64_t batch = 0; batch < update_count / batch_size; ++batch) {
        const auto generation = batch + 1;
        std::vector<JournalIndexLeafEntry> updates;
        updates.reserve(batch_size);
        const auto replacement_count = batch == 0 ? 0ull : 100ull;
        for (std::uint64_t index = 0; index < replacement_count; ++index) {
            const auto key = inserted_keys[(batch * 101 + index) %
                                           inserted_keys.size()];
            const auto type = kinds[(batch + index) % 3];
            auto update = typed_update(key, generation, append_offset, type);
            append_offset += update.record_length;
            updates.push_back(update);
            oracle[key] = lookup_kind(type);
            ++event_count;
        }
        for (std::uint64_t index = replacement_count;
             index < batch_size; ++index) {
            // 65537 is coprime with 100000, so this is a deterministic
            // permutation that creates interior inserts and split pressure.
            const auto key = (new_ordinal * 65537ull) % update_count;
            ++new_ordinal;
            const auto type = kinds[(event_count + batch) % 3];
            auto update = typed_update(key, generation, append_offset, type);
            append_offset += update.record_length;
            updates.push_back(update);
            inserted_keys.push_back(key);
            oracle.emplace(key, lookup_kind(type));
            ++event_count;
        }
        std::reverse(updates.begin(), updates.end());
        auto plan = builder->plan(OperationContext{}, view, updates,
                                  generation, 29 + batch);
        REQUIRE(plan.ok());
        const auto committed = append_offset +
            plan.value().planned_nodes().size() * kIndexNodeBytes;
        auto assigned = builder->assign_offsets(
            plan.value(), view, append_offset, sequence, committed);
        REQUIRE(assigned.ok());
        write_assigned(view, assigned.value());
        append_offset = committed;
        sequence += assigned.value().nodes.size();
        view = committed_view(view, assigned.value(), generation, committed);

        // Every published root checks its complete update batch. Release also
        // performs the full cumulative map oracle at every root (about 4.5M
        // lookups); Debug uses a fixed-seed historical sample to stay focused.
        for (const auto& update : updates) {
            const auto found = reader.value()->lookup(view,
                                                      update.block_index);
            REQUIRE(found.ok());
            REQUIRE_EQ(found.value().kind, oracle.at(update.block_index));
        }
#if defined(NDEBUG)
        for (const auto& [key, expected] : oracle) {
            const auto found = reader.value()->lookup(view, key);
            REQUIRE(found.ok());
            REQUIRE_EQ(found.value().kind, expected);
        }
#else
        for (std::uint64_t sample = 0; sample < 64; ++sample) {
            random_state = random_state * 6364136223846793005ULL +
                           1442695040888963407ULL;
            const auto key = inserted_keys[(random_state >> 16U) %
                                           inserted_keys.size()];
            const auto expected = oracle.find(key);
            REQUIRE(expected != oracle.end());
            const auto found = reader.value()->lookup(view, key);
            REQUIRE(found.ok());
            REQUIRE_EQ(found.value().kind, expected->second);
        }
#endif
        REQUIRE_EQ(reader.value()->lookup(view, update_count + 1).value().kind,
                   JournalIndexLookupKind::Absent);
    }
    REQUIRE_EQ(event_count, update_count);
    REQUIRE(view.root.offset != 0);
    REQUIRE_EQ(view.root_generation, 100ull);
    for (const auto& [key, expected] : oracle) {
        const auto found = reader.value()->lookup(view, key);
        REQUIRE(found.ok());
        REQUIRE_EQ(found.value().kind, expected);
    }
}

}  // namespace
}  // namespace myrd::persistence
