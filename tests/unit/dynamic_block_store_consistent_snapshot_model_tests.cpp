#include "test_framework.h"

#include "consistent_snapshot_model.h"
#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace {

std::vector<std::byte> block_bytes(std::uint8_t value) {
    return std::vector<std::byte>(
        static_cast<std::size_t>(
            myrd::test::consistent_model::kBlockBytes),
        std::byte{value});
}

std::vector<std::byte> initial_bytes(std::uint64_t logical_size) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(logical_size));
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        bytes[index] = std::byte{static_cast<std::uint8_t>(
            (index * 29 + (index >> 7) + 0x5A) & 0xFFu)};
    }
    return bytes;
}

std::shared_ptr<const myrd::test::FakeBackingStore> make_backing(
    const std::vector<std::byte>& bytes,
    std::uint64_t generation) {
    myrd::test::FakeBackingStoreBuilder builder(generation);
    constexpr auto block = myrd::test::consistent_model::kBlockBytes;
    const auto block_count =
        (bytes.size() + static_cast<std::size_t>(block) - 1) /
        static_cast<std::size_t>(block);
    for (std::size_t index = 0; index < block_count; ++index) {
        myrd::test::FakeBackingStore::BlockData data{};
        const auto source_offset = index * static_cast<std::size_t>(block);
        const auto valid_bytes = (std::min)(
            static_cast<std::size_t>(block),
            bytes.size() - source_offset);
        std::copy_n(
            bytes.begin() + static_cast<std::ptrdiff_t>(source_offset),
            valid_bytes,
            data.begin());
        builder.data_block(index, data);
    }
    return builder.build();
}

std::unique_ptr<myrd::DynamicBlockStore> make_store(
    std::uint64_t logical_size,
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t generation) {
    constexpr auto block = myrd::test::consistent_model::kBlockBytes;
    auto created = myrd::DynamicBlockStore::create(
        myrd::DynamicBlockStoreCreateArgs{
            logical_size,
            static_cast<std::uint32_t>(
                myrd::test::consistent_model::kSectorBytes),
            myrd::DynamicBlockStoreOptions{
                32 * block,
                64 * block,
                16 * block,
                4 * block,
                2,
                64,
                64 * myrd::kMinimumSnapshotCowBytes,
                std::chrono::milliseconds{250},
            },
            myrd::InitialBlockState::BackingUnresident,
            std::move(backing),
            generation,
        });
    REQUIRE(created.ok());
    return created.move_value();
}

myrd::DirtyRecordKind native_kind(
    myrd::test::consistent_model::Kind kind) {
    switch (kind) {
    case myrd::test::consistent_model::Kind::Data:
        return myrd::DirtyRecordKind::Data;
    case myrd::test::consistent_model::Kind::Zero:
        return myrd::DirtyRecordKind::Zero;
    case myrd::test::consistent_model::Kind::Discard:
        return myrd::DirtyRecordKind::Discard;
    }
    std::terminate();
}

void require_equal_records(
    const std::vector<myrd::DirtyBlockSnapshot>& actual,
    const std::vector<myrd::test::consistent_model::SnapshotRecord>& expected) {
    REQUIRE_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        REQUIRE_EQ(actual[index].block_index, expected[index].block_index);
        REQUIRE_EQ(actual[index].version, expected[index].version);
        REQUIRE_EQ(actual[index].kind, native_kind(expected[index].kind));
        REQUIRE_EQ(actual[index].base_generation,
                   expected[index].base_generation);
        REQUIRE_EQ(actual[index].data, expected[index].data);
    }
}

bool records_equal(
    const std::vector<myrd::DirtyBlockSnapshot>& actual,
    const std::vector<myrd::test::consistent_model::SnapshotRecord>& expected) {
    if (actual.size() != expected.size()) {
        return false;
    }
    for (std::size_t index = 0; index < actual.size(); ++index) {
        if (actual[index].block_index != expected[index].block_index ||
            actual[index].version != expected[index].version ||
            actual[index].kind != native_kind(expected[index].kind) ||
            actual[index].base_generation !=
                expected[index].base_generation ||
            actual[index].data != expected[index].data) {
            return false;
        }
    }
    return true;
}

[[noreturn]] void fail_schedule(
    std::uint64_t seed,
    std::size_t index,
    const myrd::test::consistent_model::Operation& operation,
    std::string_view message) {
    std::cerr << "M2_1_MODEL_FAILURE seed=0x" << std::hex << seed
              << std::dec << " "
              << myrd::test::consistent_model::describe_operation(
                     index, operation)
              << " message=" << message << std::endl;
    std::_Exit(1);
}

void expect_schedule(
    bool condition,
    std::uint64_t seed,
    std::size_t index,
    const myrd::test::consistent_model::Operation& operation,
    std::string_view message) {
    if (!condition) {
        fail_schedule(seed, index, operation, message);
    }
}

void run_schedule(std::uint64_t seed, std::size_t operation_count) {
    using namespace myrd::test::consistent_model;
    constexpr std::uint64_t generation = 7;
    const auto schedule = generate_schedule(seed, operation_count);
    const auto initial = initial_bytes(schedule.logical_size_bytes());
    auto store = make_store(
        schedule.logical_size_bytes(),
        make_backing(initial, generation),
        generation);
    Model model(schedule.logical_size_bytes(), initial, generation);

    std::optional<myrd::ConsistentSnapshotToken> actual_token;
    std::optional<CutToken> expected_token;
    myrd::DirtyScanCursor actual_cursor{};
    std::uint64_t expected_cursor = 0;
    std::vector<myrd::DirtyBlockSnapshot> actual_union;
    std::vector<SnapshotRecord> expected_union;
    std::vector<SnapshotRecord> expected_cut;

    for (std::size_t index = 0; index < schedule.operations.size(); ++index) {
        const auto& operation = schedule.operations[index];
        switch (operation.kind) {
        case OperationKind::Write: {
            const auto payload = operation_payload(operation);
            const auto actual = store->write(
                myrd::OperationContext{}, operation.offset, payload);
            const bool expected = model.write(operation.offset, payload);
            expect_schedule(
                actual.ok() == expected,
                seed,
                index,
                operation,
                "write result diverged");
            break;
        }
        case OperationKind::Zero:
        case OperationKind::Discard: {
            const std::array<myrd::DataRange, 1> ranges{{
                {operation.offset, operation.length}}};
            const auto actual = operation.kind == OperationKind::Zero
                ? store->zero(myrd::OperationContext{}, ranges)
                : store->unmap(myrd::OperationContext{}, ranges);
            const bool expected = model.zero(
                operation.offset,
                operation.length,
                operation.kind == OperationKind::Zero
                    ? Kind::Zero
                    : Kind::Discard);
            expect_schedule(
                actual.ok() == expected,
                seed,
                index,
                operation,
                "zero/discard result diverged");
            break;
        }
        case OperationKind::Begin: {
            const auto actual = store->begin_consistent_snapshot(
                myrd::OperationContext{});
            const auto expected = model.begin_cut();
            expect_schedule(
                actual.ok() == expected.has_value(),
                seed,
                index,
                operation,
                "begin lifecycle diverged");
            if (actual.ok()) {
                actual_token = actual.value();
                expected_token = expected;
                actual_cursor = {};
                expected_cursor = 0;
                actual_union.clear();
                expected_union.clear();
                expected_cut = *model.cut_records(*expected_token);
            }
            break;
        }
        case OperationKind::Copy:
            if (actual_token.has_value()) {
                const auto actual = store->copy_consistent_snapshot_batch(
                    myrd::OperationContext{},
                    *actual_token,
                    actual_cursor,
                    operation.max_payload_bytes);
                const auto expected = model.copy_cut(
                    *expected_token,
                    expected_cursor,
                    operation.max_payload_bytes,
                    2);
                expect_schedule(
                    actual.ok() && expected.has_value(),
                    seed,
                    index,
                    operation,
                    "copy lifecycle diverged");
                expect_schedule(
                    records_equal(actual.value(), *expected),
                    seed,
                    index,
                    operation,
                    "copy batch differs from cut");
                expect_schedule(
                    actual_cursor.next_block_index == expected_cursor,
                    seed,
                    index,
                    operation,
                    "copy cursor diverged");
                actual_union.insert(
                    actual_union.end(),
                    actual.value().begin(),
                    actual.value().end());
                expected_union.insert(
                    expected_union.end(),
                    expected->begin(),
                    expected->end());
            }
            break;
        case OperationKind::Finish:
            if (actual_token.has_value()) {
                const auto actual =
                    store->finish_consistent_snapshot(*actual_token);
                const bool expected =
                    model.finish_cut(*expected_token, expected_cursor);
                expect_schedule(
                    actual.ok() == expected,
                    seed,
                    index,
                    operation,
                    "finish lifecycle diverged");
                if (expected) {
                    expect_schedule(
                        records_equal(actual_union, expected_union) &&
                            expected_union == expected_cut,
                        seed,
                        index,
                        operation,
                        "finished union differs from immutable cut");
                    actual_token.reset();
                    expected_token.reset();
                }
            }
            break;
        case OperationKind::Abort:
            if (actual_token.has_value()) {
                const auto actual =
                    store->abort_consistent_snapshot(*actual_token);
                const bool expected = model.abort_cut(*expected_token);
                expect_schedule(
                    actual.ok() && expected,
                    seed,
                    index,
                    operation,
                    "abort lifecycle diverged");
                actual_token.reset();
                expected_token.reset();
            }
            break;
        }
    }
    if (actual_token.has_value()) {
        const auto actual = store->abort_consistent_snapshot(*actual_token);
        if (!actual.ok() || !model.abort_cut(*expected_token)) {
            fail_schedule(
                seed,
                schedule.operations.size() - 1,
                schedule.operations.back(),
                "final cleanup abort diverged");
        }
    }
    if (!store->validate_invariants_for_test().ok()) {
        fail_schedule(
            seed,
            schedule.operations.size() - 1,
            schedule.operations.back(),
            "store invariant failed after schedule");
    }
}

} // namespace

TEST_CASE(m2_1_task08_model_restates_geometry_and_starts_clean) {
    myrd::test::consistent_model::Model model(
        2 * myrd::test::consistent_model::kBlockBytes,
        7);

    REQUIRE_EQ(model.logical_size_bytes(),
               2 * myrd::test::consistent_model::kBlockBytes);
    REQUIRE_EQ(model.block_count(), 2ull);
    REQUIRE_EQ(model.backing_generation(), 7ull);
    REQUIRE_EQ(model.dirty_block_count(), 0ull);
    REQUIRE(!model.active_cut().has_value());
}

TEST_CASE(m2_1_task08_begin_copies_one_immutable_whole_disk_cut) {
    using myrd::test::consistent_model::Kind;
    using myrd::test::consistent_model::Model;
    constexpr auto block = myrd::test::consistent_model::kBlockBytes;
    Model model(4 * block, 7);

    const auto old_data = block_bytes(0x31);
    REQUIRE(model.write(0, old_data));
    REQUIRE(model.zero(block, block, Kind::Zero));
    REQUIRE(model.zero(2 * block, block, Kind::Discard));

    const auto begun = model.begin_cut();
    REQUIRE(begun.has_value());
    REQUIRE_EQ(model.active_cut(), begun);

    const auto replacement = block_bytes(0x42);
    REQUIRE(model.write(0, replacement));
    REQUIRE(model.write(3 * block, block_bytes(0x53)));

    const auto* cut = model.cut_records(*begun);
    REQUIRE(cut != nullptr);
    REQUIRE_EQ(cut->size(), 3ull);
    REQUIRE_EQ((*cut)[0].block_index, 0ull);
    REQUIRE_EQ((*cut)[0].version, 1ull);
    REQUIRE_EQ((*cut)[0].kind, Kind::Data);
    REQUIRE_EQ((*cut)[0].base_generation, 7ull);
    REQUIRE_EQ((*cut)[0].data, old_data);
    REQUIRE_EQ((*cut)[1].block_index, 1ull);
    REQUIRE_EQ((*cut)[1].kind, Kind::Zero);
    REQUIRE((*cut)[1].data.empty());
    REQUIRE_EQ((*cut)[2].block_index, 2ull);
    REQUIRE_EQ((*cut)[2].kind, Kind::Discard);
    REQUIRE((*cut)[2].data.empty());
    REQUIRE_EQ(model.dirty_block_count(), 4ull);
}

TEST_CASE(m2_1_task08_schedule_is_seed_only_deterministic_and_complete) {
    using namespace myrd::test::consistent_model;
    constexpr std::uint64_t seed = 0x00C0FFEEull;
    constexpr std::size_t operation_count = 10'000;

    const auto first = generate_schedule(seed, operation_count);
    const auto replay = generate_schedule(seed, operation_count);
    const auto different = generate_schedule(seed + 1, operation_count);

    REQUIRE_EQ(first, replay);
    REQUIRE(first != different);
    REQUIRE_EQ(first.seed, seed);
    REQUIRE(first.block_count >= 2);
    REQUIRE(first.block_count <= 64);
    REQUIRE_EQ(first.operations.size(), operation_count);

    std::array<std::uint64_t, 7> kinds{};
    for (const auto& operation : first.operations) {
        ++kinds[static_cast<std::size_t>(operation.kind)];
        if (operation.kind == OperationKind::Write ||
            operation.kind == OperationKind::Zero ||
            operation.kind == OperationKind::Discard) {
            REQUIRE(operation.length != 0);
            REQUIRE_EQ(operation.offset % kSectorBytes, 0ull);
            REQUIRE_EQ(operation.length % kSectorBytes, 0ull);
            REQUIRE(operation.offset <= first.logical_size_bytes());
            REQUIRE(operation.length <=
                    first.logical_size_bytes() - operation.offset);
        }
    }
    for (const auto count : kinds) {
        REQUIRE(count != 0);
    }
    REQUIRE(!describe_operation(0, first.operations.front()).empty());
    REQUIRE_EQ(operation_payload(first.operations.front()),
               operation_payload(replay.operations.front()));
}

TEST_CASE(m2_1_task08_model_batches_one_cut_and_invalidates_abort) {
    using namespace myrd::test::consistent_model;
    Model model(4 * kBlockBytes, 7);
    REQUIRE(model.write(0, block_bytes(0x61)));
    REQUIRE(model.zero(kBlockBytes, kBlockBytes, Kind::Zero));
    REQUIRE(model.zero(2 * kBlockBytes, kBlockBytes, Kind::Discard));
    const auto token = model.begin_cut();
    REQUIRE(token.has_value());

    std::uint64_t cursor = 0;
    const auto first = model.copy_cut(
        *token, cursor, kBlockBytes, 2);
    REQUIRE(first.has_value());
    REQUIRE_EQ(first->size(), 2ull);
    REQUIRE_EQ((*first)[0].block_index, 0ull);
    REQUIRE_EQ((*first)[1].block_index, 1ull);
    REQUIRE_EQ(cursor, 2ull);
    REQUIRE(!model.finish_cut(*token, cursor));

    const auto second = model.copy_cut(
        *token, cursor, kBlockBytes, 2);
    REQUIRE(second.has_value());
    REQUIRE_EQ(second->size(), 1ull);
    REQUIRE_EQ(second->front().block_index, 2ull);
    REQUIRE_EQ(cursor, model.block_count());
    REQUIRE(model.finish_cut(*token, cursor));
    REQUIRE(!model.active_cut().has_value());
    REQUIRE(model.cut_records(*token) == nullptr);

    const auto aborted = model.begin_cut();
    REQUIRE(aborted.has_value());
    cursor = 0;
    REQUIRE(model.copy_cut(*aborted, cursor, kBlockBytes, 1).has_value());
    REQUIRE(model.abort_cut(*aborted));
    REQUIRE(!model.copy_cut(
        *aborted, cursor, kBlockBytes, 1).has_value());
    REQUIRE(!model.abort_cut(*aborted));
}

TEST_CASE(m2_1_task08_directed_store_union_equals_independent_cut) {
    using namespace myrd::test::consistent_model;
    constexpr std::uint64_t generation = 7;
    const auto logical_size = 4 * kBlockBytes + kSectorBytes;
    const auto initial = initial_bytes(logical_size);
    auto store = make_store(
        logical_size, make_backing(initial, generation), generation);
    Model model(logical_size, initial, generation);

    const auto old_data = block_bytes(0x71);
    REQUIRE(store->write(myrd::OperationContext{}, 0, old_data).ok());
    REQUIRE(model.write(0, old_data));
    const std::array<myrd::DataRange, 1> zero_range{{
        {kBlockBytes, kBlockBytes}}};
    REQUIRE(store->zero(myrd::OperationContext{}, zero_range).ok());
    REQUIRE(model.zero(kBlockBytes, kBlockBytes, Kind::Zero));
    const std::array<myrd::DataRange, 1> discard_range{{
        {2 * kBlockBytes, kBlockBytes}}};
    REQUIRE(store->unmap(myrd::OperationContext{}, discard_range).ok());
    REQUIRE(model.zero(2 * kBlockBytes, kBlockBytes, Kind::Discard));
    const std::vector<std::byte> final_sector(
        static_cast<std::size_t>(kSectorBytes), std::byte{0x7F});
    REQUIRE(store->write(
        myrd::OperationContext{}, 4 * kBlockBytes, final_sector).ok());
    REQUIRE(model.write(4 * kBlockBytes, final_sector));

    const auto actual_token =
        store->begin_consistent_snapshot(myrd::OperationContext{});
    const auto expected_token = model.begin_cut();
    REQUIRE(actual_token.ok());
    REQUIRE(expected_token.has_value());
    const auto expected_cut = *model.cut_records(*expected_token);

    REQUIRE(store->write(
        myrd::OperationContext{}, 0, block_bytes(0x82)).ok());
    REQUIRE(model.write(0, block_bytes(0x82)));
    REQUIRE(store->write(
        myrd::OperationContext{}, 3 * kBlockBytes, block_bytes(0x93)).ok());
    REQUIRE(model.write(3 * kBlockBytes, block_bytes(0x93)));

    myrd::DirtyScanCursor actual_cursor{};
    std::uint64_t expected_cursor = 0;
    std::vector<myrd::DirtyBlockSnapshot> actual_union;
    std::vector<SnapshotRecord> expected_union;
    while (actual_cursor.next_block_index < model.block_count()) {
        const auto actual = store->copy_consistent_snapshot_batch(
            myrd::OperationContext{},
            actual_token.value(),
            actual_cursor,
            kBlockBytes);
        const auto expected = model.copy_cut(
            *expected_token, expected_cursor, kBlockBytes, 2);
        REQUIRE(actual.ok());
        REQUIRE(expected.has_value());
        require_equal_records(actual.value(), *expected);
        REQUIRE_EQ(actual_cursor.next_block_index, expected_cursor);
        actual_union.insert(
            actual_union.end(), actual.value().begin(), actual.value().end());
        expected_union.insert(
            expected_union.end(), expected->begin(), expected->end());
    }
    require_equal_records(actual_union, expected_union);
    REQUIRE_EQ(expected_union, expected_cut);
    REQUIRE_EQ(expected_union.size(), 4ull);
    REQUIRE_EQ(expected_union.back().block_index, 4ull);
    REQUIRE_EQ(expected_union.back().data.size(),
               static_cast<std::size_t>(kBlockBytes));
    REQUIRE(std::all_of(
        expected_union.back().data.begin() +
            static_cast<std::ptrdiff_t>(kSectorBytes),
        expected_union.back().data.end(),
        [](std::byte value) { return value == std::byte{0}; }));

    REQUIRE(store->finish_consistent_snapshot(actual_token.value()).ok());
    REQUIRE(model.finish_cut(*expected_token, expected_cursor));
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(m2_1_task08_generated_schedule_matches_independent_cut_model) {
    run_schedule(0xDEADBEEFull, 2'000);
}

TEST_CASE(m2_1_task08_abort_after_partial_progress_invalidates_both_tokens) {
    using namespace myrd::test::consistent_model;
    constexpr std::uint64_t generation = 7;
    const auto initial = initial_bytes(2 * kBlockBytes);
    auto store = make_store(
        2 * kBlockBytes, make_backing(initial, generation), generation);
    Model model(2 * kBlockBytes, initial, generation);
    REQUIRE(store->write(
        myrd::OperationContext{}, 0, block_bytes(0xA1)).ok());
    REQUIRE(model.write(0, block_bytes(0xA1)));
    REQUIRE(store->write(
        myrd::OperationContext{}, kBlockBytes, block_bytes(0xB2)).ok());
    REQUIRE(model.write(kBlockBytes, block_bytes(0xB2)));

    const auto actual_token =
        store->begin_consistent_snapshot(myrd::OperationContext{});
    const auto expected_token = model.begin_cut();
    REQUIRE(actual_token.ok());
    REQUIRE(expected_token.has_value());
    myrd::DirtyScanCursor actual_cursor{};
    std::uint64_t expected_cursor = 0;
    const auto actual = store->copy_consistent_snapshot_batch(
        myrd::OperationContext{},
        actual_token.value(),
        actual_cursor,
        kBlockBytes);
    const auto expected = model.copy_cut(
        *expected_token, expected_cursor, kBlockBytes, 2);
    REQUIRE(actual.ok());
    REQUIRE(expected.has_value());
    require_equal_records(actual.value(), *expected);
    REQUIRE_EQ(actual_cursor.next_block_index, 1ull);
    REQUIRE_EQ(expected_cursor, 1ull);

    REQUIRE(store->abort_consistent_snapshot(actual_token.value()).ok());
    REQUIRE(model.abort_cut(*expected_token));
    const auto stale_actual = store->copy_consistent_snapshot_batch(
        myrd::OperationContext{},
        actual_token.value(),
        actual_cursor,
        kBlockBytes);
    const auto stale_expected = model.copy_cut(
        *expected_token, expected_cursor, kBlockBytes, 2);
    REQUIRE(!stale_actual.ok());
    REQUIRE(!stale_expected.has_value());
    REQUIRE_EQ(actual_cursor.next_block_index, 1ull);
    REQUIRE_EQ(expected_cursor, 1ull);
    REQUIRE(store->validate_invariants_for_test().ok());
}
