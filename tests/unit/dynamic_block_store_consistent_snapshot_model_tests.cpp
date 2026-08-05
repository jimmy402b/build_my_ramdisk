#include "test_framework.h"

#include "consistent_snapshot_model.h"
#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/eviction_manager.h"

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

// SHA256("937e688|T08|i") low 64 bits, i in [0, 99]. The source
// commit is the frozen Task08 starting point, not the implementation under
// test, so later production changes cannot silently choose easier seeds.
constexpr std::array<std::uint64_t, 100> kGeneratedSeeds{
    0x7BFC'A61D'89EB'62CEull, 0x3D23'D6B0'C038'10F9ull, 0xE3FF'8569'F1E3'C11Bull, 0x3D56'9F15'581D'EBBCull, 0x3C9D'5FFB'2F31'AC73ull,
    0x5AC9'5165'85D8'6425ull, 0xCBE1'FDE7'0C03'5616ull, 0x4646'580D'5157'C248ull, 0x53A2'ED7F'A158'E195ull, 0xB101'D3D5'9D72'2816ull,
    0x862D'9761'65F3'46C5ull, 0x260A'29D3'6DA6'4F4Bull, 0xBA8F'FD86'DEEF'CC52ull, 0x93F5'F0F5'0023'EAAEull, 0x7F97'996B'7DBB'9511ull,
    0x33BA'CA2B'E35C'1A1Full, 0xB985'7B3C'9C25'82A4ull, 0xAB8A'20DB'43B9'A25Dull, 0x7B36'19EB'3282'EA9Dull, 0xEE2F'66CB'1ABE'3F4Cull,
    0x9637'7941'999C'7F8Aull, 0x45D5'8F0B'21FA'E743ull, 0x3256'C049'BE80'6810ull, 0x0D73'E547'01D9'A4B1ull, 0x569A'3DDF'0C92'0B44ull,
    0x3D28'ACF4'762A'738Full, 0x472B'4803'A50E'181Dull, 0x68EC'E2D0'57EE'F63Full, 0xCD9C'CA9A'7321'2889ull, 0x6A1A'CC3C'FCE2'0687ull,
    0xF8A2'CCE8'9BEE'5821ull, 0xC968'3A3D'4850'E833ull, 0x70B8'1501'C107'3429ull, 0x7FE3'8EA0'6C35'A4E8ull, 0x0AE6'7333'5D0B'54C7ull,
    0x376F'7FB7'3111'BC65ull, 0x4D03'F3C2'9652'720Dull, 0xA036'6696'1A53'7C45ull, 0x09D5'6F74'87F7'AAECull, 0xF2F9'86E5'F606'4813ull,
    0x72BE'DF94'318F'D4B4ull, 0x57D7'3817'956E'A5F7ull, 0x47E5'4C6A'468C'98C8ull, 0x0A8A'23B3'C161'6217ull, 0x1A09'A6A5'2547'D602ull,
    0xEC1A'A32D'F22E'4565ull, 0xF612'2410'577F'1084ull, 0x61A7'DD74'0E4C'E13Full, 0x0D21'97BE'F44A'2409ull, 0x2E85'1F06'DCC0'E078ull,
    0x5494'30C6'AD8C'EF34ull, 0x0F8C'2314'4180'8275ull, 0x4756'3103'DEEF'02E0ull, 0x5742'EBBC'C862'C93Dull, 0x50F6'602D'D172'66DBull,
    0x8BE6'712B'89A7'25D6ull, 0xABDC'605C'6D3E'00EEull, 0x157E'98AE'0DCC'3F31ull, 0x990B'4144'334C'B812ull, 0xCD33'0764'0168'E7BEull,
    0x850D'03E1'FF73'ADF9ull, 0xDAF8'7634'764D'6C38ull, 0x51A8'6E86'29EC'3CB0ull, 0xE3B5'AA5B'6A8C'C45Cull, 0xAEBF'8FAA'2DA2'0E47ull,
    0xE139'AF32'5776'6BB7ull, 0xA3FF'B59D'D41F'90AEull, 0x228A'654F'8EA8'52A9ull, 0x6E01'7BC5'25C3'2CD9ull, 0x9887'6A89'EF5B'C911ull,
    0x140F'D6EB'5EBD'BEF7ull, 0x403B'5562'FD0B'F79Dull, 0x69EF'809F'90DD'781Cull, 0xE21E'4092'CE48'C403ull, 0xDF25'7DAB'D856'0B31ull,
    0x007B'4840'74A9'9A84ull, 0x3FB9'07F0'2210'026Aull, 0x64B9'7E3C'ECDF'69B3ull, 0xADB9'1537'8AC5'4593ull, 0x8DCC'6F6A'B51B'73B6ull,
    0xA238'0819'AF59'FA9Eull, 0x121D'9A92'5AC7'3A07ull, 0xB166'D6E7'C311'AD0Aull, 0x37E6'1151'1C3F'9707ull, 0x1DD5'31AE'DA6D'727Cull,
    0xC168'FD61'BBAB'41DEull, 0xB5D8'E810'06D7'A71Cull, 0x27C4'5AE5'48DF'C1C1ull, 0xA1DD'9B5F'3C99'3E43ull, 0x4701'49C9'B24A'DA53ull,
    0x18F2'A8BF'D1B7'87F9ull, 0xBACE'966D'70C7'30B6ull, 0xF0B1'C8F3'DD03'2625ull, 0xCFD5'AF32'99BB'7110ull, 0xEAD3'00CD'35B3'5D72ull,
    0x9AAC'F08C'D5E1'F0D7ull, 0xE3C0'D439'D977'3FC3ull, 0x9473'F179'4C71'7CA7ull, 0x9CC7'DC3D'55B9'2614ull, 0xD806'F0A2'92EA'6208ull,
};

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
        case OperationKind::Read:
        case OperationKind::Load: {
            std::vector<std::byte> actual_bytes(
                static_cast<std::size_t>(operation.length));
            const auto actual = store->read(
                myrd::OperationContext{},
                operation.offset,
                actual_bytes);
            const auto expected = model.read(
                operation.offset, operation.length);
            expect_schedule(
                actual.ok() == expected.has_value(),
                seed,
                index,
                operation,
                "read/load result diverged");
            expect_schedule(
                actual.ok() && actual_bytes == *expected,
                seed,
                index,
                operation,
                "read/load bytes diverged");
            break;
        }
        case OperationKind::Evict: {
            const auto pass =
                myrd::DynamicBlockStoreTestAccess::run_eviction_sweep(
                    *store, 0);
            expect_schedule(
                pass.invariant_failures == 0,
                seed,
                index,
                operation,
                "eviction reported an invariant failure");
            break;
        }
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

    std::array<std::uint64_t, 10> kinds{};
    bool saw_cross_block_range = false;
    for (const auto& operation : first.operations) {
        ++kinds[static_cast<std::size_t>(operation.kind)];
        if (operation.kind == OperationKind::Write ||
            operation.kind == OperationKind::Zero ||
            operation.kind == OperationKind::Discard ||
            operation.kind == OperationKind::Read ||
            operation.kind == OperationKind::Load) {
            REQUIRE(operation.length != 0);
            REQUIRE_EQ(operation.offset % kSectorBytes, 0ull);
            REQUIRE_EQ(operation.length % kSectorBytes, 0ull);
            REQUIRE(operation.offset <= first.logical_size_bytes());
            REQUIRE(operation.length <=
                    first.logical_size_bytes() - operation.offset);
            if (operation.offset / kBlockBytes !=
                (operation.offset + operation.length - 1) / kBlockBytes) {
                saw_cross_block_range = true;
            }
        }
    }
    for (const auto count : kinds) {
        REQUIRE(count != 0);
    }
    REQUIRE(saw_cross_block_range);
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

TEST_CASE(m2_1_task08_model_owns_token_cursor_and_finish_state) {
    using namespace myrd::test::consistent_model;
    Model model(3 * kBlockBytes, 7);
    Model other_model(3 * kBlockBytes, 7);
    REQUIRE(model.write(0, block_bytes(0x64)));
    REQUIRE(model.write(kBlockBytes, block_bytes(0x65)));
    const auto token = model.begin_cut();
    const auto other_token = other_model.begin_cut();
    REQUIRE(token.has_value());
    REQUIRE(other_token.has_value());
    REQUIRE(*token != *other_token);

    std::uint64_t forged_cursor = 2;
    REQUIRE(!model.copy_cut(
        *token, forged_cursor, kBlockBytes, 1).has_value());
    REQUIRE_EQ(forged_cursor, 2ull);
    REQUIRE(!model.finish_cut(*token, model.block_count()));

    std::uint64_t cursor = 0;
    const auto first = model.copy_cut(
        *token, cursor, kBlockBytes, 1);
    REQUIRE(first.has_value());
    REQUIRE_EQ(first->size(), 1ull);
    REQUIRE_EQ(cursor, 1ull);

    std::uint64_t rolled_back_cursor = 0;
    REQUIRE(!model.copy_cut(
        *token, rolled_back_cursor, kBlockBytes, 1).has_value());
    REQUIRE_EQ(rolled_back_cursor, 0ull);
    REQUIRE(!model.finish_cut(*token, model.block_count()));

    std::uint64_t foreign_cursor = cursor;
    REQUIRE(!model.copy_cut(
        *other_token, foreign_cursor, kBlockBytes, 1).has_value());
    REQUIRE_EQ(foreign_cursor, cursor);

    const auto second = model.copy_cut(
        *token, cursor, kBlockBytes, 1);
    REQUIRE(second.has_value());
    REQUIRE_EQ(second->size(), 1ull);
    REQUIRE_EQ(cursor, 2ull);
    const auto exhausted = model.copy_cut(
        *token, cursor, kBlockBytes, 1);
    REQUIRE(exhausted.has_value());
    REQUIRE(exhausted->empty());
    REQUIRE_EQ(cursor, model.block_count());
    REQUIRE(model.finish_cut(*token, cursor));
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

TEST_CASE(m2_1_task08_reducer_keeps_failure_and_only_shrinks_trace) {
    using namespace myrd::test::consistent_model;
    auto schedule = generate_schedule(0x1234, 8);
    schedule.operations[0] = Operation{
        OperationKind::Begin, 0, 0, 1, kBlockBytes, 2};
    schedule.operations[1] = Operation{
        OperationKind::Write, 0, 8 * kSectorBytes, 2, kBlockBytes, 2};
    schedule.operations[2] = Operation{
        OperationKind::Write,
        kBlockBytes,
        8 * kSectorBytes,
        0xBAD5EED,
        kBlockBytes,
        2};
    const auto predicate = [](const Schedule& candidate) {
        return std::any_of(
            candidate.operations.begin(),
            candidate.operations.end(),
            [](const Operation& operation) {
                return operation.kind == OperationKind::Write &&
                       operation.payload_seed == 0xBAD5EED;
            });
    };

    const auto reduced = reduce_failure(schedule, predicate);
    REQUIRE(reduced.has_value());
    REQUIRE(predicate(*reduced));
    REQUIRE_EQ(reduced->seed, schedule.seed);
    REQUIRE_EQ(reduced->block_count, schedule.block_count);
    REQUIRE_EQ(reduced->operations.size(), 3ull);
    REQUIRE_EQ(reduced->operations[0], schedule.operations[0]);
    REQUIRE_EQ(reduced->operations[1], schedule.operations[1]);
    REQUIRE_EQ(reduced->operations[2].kind, OperationKind::Write);
    REQUIRE_EQ(reduced->operations[2].payload_seed, 0xBAD5EEDull);
    REQUIRE_EQ(reduced->operations[2].offset, kBlockBytes);
    REQUIRE_EQ(reduced->operations[2].length, kSectorBytes);

    REQUIRE(!reduce_failure(
        schedule, [](const Schedule&) { return false; }).has_value());
}

TEST_CASE(m2_1_task08_one_hundred_generated_seeds_match_cut_model) {
    for (const auto seed : kGeneratedSeeds) {
        run_schedule(seed, 1'000);
    }
}

TEST_CASE(m2_1_task08_quick_profile_fixed_seeds_match_cut_model) {
    constexpr std::array<std::uint64_t, 4> seeds{
        0,
        1,
        0x00C0FFEEull,
        0xDEADBEEFull,
    };
    for (const auto seed : seeds) {
        run_schedule(seed, 1'000);
    }
}
