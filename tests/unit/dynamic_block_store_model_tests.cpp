#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "test_framework.h"

#include "fake_backing_store.h"
#include "reference_block_model.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/eviction_manager.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <iterator>
#include <memory>
#include <optional>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kBlock =
    myrd::reference::kReferenceBlockBytes;
constexpr std::uint64_t kSector =
    myrd::reference::kReferenceSectorBytes;
constexpr std::uint64_t kCanonicalSeed = 0x4D59'5244'4D32'4131ull;
constexpr std::uint64_t kOperationCount = 100'000;
constexpr std::uint64_t kMainBlockCount = 65;
constexpr std::size_t kSnapshotRecordLimit = 7;

class DeterministicRandom final {
public:
    explicit DeterministicRandom(std::uint64_t seed) noexcept : state_(seed) {}

    std::uint64_t next() noexcept {
        auto value = (state_ += 0x9E37'79B9'7F4A'7C15ull);
        value = (value ^ (value >> 30)) * 0xBF58'476D'1CE4'E5B9ull;
        value = (value ^ (value >> 27)) * 0x94D0'49BB'1331'11EBull;
        return value ^ (value >> 31);
    }

private:
    std::uint64_t state_;
};

class HarnessContext final {
public:
    explicit HarnessContext(std::uint64_t seed) : seed_(seed) {}

    void set_operation(std::uint64_t index, std::string operation) {
        index_ = index;
        history_.push_back(std::move(operation));
        if (history_.size() > 12) {
            history_.pop_front();
        }
    }

    [[noreturn]] void fail(const std::string& message,
                           const myrd::reference::BlockModel* model = nullptr) const {
        std::cerr << "M2_MODEL_FAILURE seed=0x" << std::hex << seed_
                  << std::dec << " operation_index=" << index_
                  << " message=" << message << '\n';
        if (model != nullptr) {
            const auto gauges = model->gauges();
            std::cerr << "M2_MODEL_STATE generation="
                      << model->backing_generation()
                      << " dirty=" << model->dirty_blocks()
                      << " committed=" << gauges.committed_blocks
                      << " zero_clean=" << gauges.zero_clean_blocks
                      << " zero_dirty=" << gauges.zero_dirty_blocks << '\n';
        }
        for (const auto& operation : history_) {
            std::cerr << "M2_MODEL_HISTORY " << operation << '\n';
        }
        std::_Exit(1);
    }

    void expect(bool condition,
                const std::string& message,
                const myrd::reference::BlockModel* model = nullptr) const {
        if (!condition) {
            fail(message, model);
        }
    }

private:
    std::uint64_t seed_{};
    std::uint64_t index_{};
    std::deque<std::string> history_;
};

struct Range {
    std::uint64_t offset{};
    std::uint64_t length{};
};

struct RunReport {
    std::uint64_t seed{};
    std::uint64_t completed_operations{};
    std::uint64_t digest{};
};

struct OperationCoverage {
    std::uint64_t reads{};
    std::uint64_t writes{};
    std::uint64_t zeroes{};
    std::uint64_t unmaps{};
    std::uint64_t dirty_scans{};
    std::uint64_t rejected_acks{};
    std::uint64_t zero_length_rejections{};
    std::uint64_t checkpoints{};
    std::uint64_t pressure_probes{};
    std::uint64_t sparse_distant_probes{};

    std::uint64_t full_zeroes{};
    std::uint64_t partial_zeroes{};
    std::uint64_t full_unmaps{};
    std::uint64_t partial_unmaps{};
    std::uint64_t cross_block_ranges{};
    std::uint64_t last_sector_ranges{};
    std::uint64_t repeated_hot_block_ranges{};

    std::uint64_t total() const noexcept {
        return reads + writes + zeroes + unmaps + dirty_scans +
               rejected_acks + zero_length_rejections + checkpoints +
               pressure_probes + sparse_distant_probes;
    }
};

myrd::DirtyRecordKind to_native(myrd::reference::DirtyKind kind) {
    switch (kind) {
    case myrd::reference::DirtyKind::Data:
        return myrd::DirtyRecordKind::Data;
    case myrd::reference::DirtyKind::Zero:
        return myrd::DirtyRecordKind::Zero;
    case myrd::reference::DirtyKind::Discard:
        return myrd::DirtyRecordKind::Discard;
    }
    std::terminate();
}

myrd::reference::DirtyKind to_reference(myrd::DirtyRecordKind kind) {
    switch (kind) {
    case myrd::DirtyRecordKind::Data:
        return myrd::reference::DirtyKind::Data;
    case myrd::DirtyRecordKind::Zero:
        return myrd::reference::DirtyKind::Zero;
    case myrd::DirtyRecordKind::Discard:
        return myrd::reference::DirtyKind::Discard;
    }
    std::terminate();
}

const char* kind_name(myrd::reference::DirtyKind kind) noexcept {
    switch (kind) {
    case myrd::reference::DirtyKind::Data:
        return "data";
    case myrd::reference::DirtyKind::Zero:
        return "zero";
    case myrd::reference::DirtyKind::Discard:
        return "discard";
    }
    return "unknown";
}

std::vector<std::byte> make_initial_bytes(std::uint64_t block_count,
                                          std::uint64_t seed) {
    std::vector<std::byte> bytes(
        static_cast<std::size_t>(block_count * kBlock));
    for (std::uint64_t block = 0; block < block_count; ++block) {
        for (std::uint64_t offset = 0; offset < kBlock; ++offset) {
            auto value = seed + block * 131 + offset * 17 + (offset >> 8);
            value ^= value >> 23;
            bytes[static_cast<std::size_t>(block * kBlock + offset)] =
                std::byte{static_cast<std::uint8_t>(value & 0xFFu)};
        }
    }
    return bytes;
}

std::shared_ptr<const myrd::test::FakeBackingStore> make_backing(
    const std::vector<std::byte>& bytes,
    std::uint64_t seed) {
    myrd::test::FakeBackingStoreBuilder builder(seed);
    const auto block_count =
        (bytes.size() + static_cast<std::size_t>(kBlock) - 1) /
        static_cast<std::size_t>(kBlock);
    for (std::size_t block = 0; block < block_count; ++block) {
        myrd::test::FakeBackingStore::BlockData data{};
        const auto first = bytes.begin() +
            static_cast<std::ptrdiff_t>(block * kBlock);
        const auto valid_bytes = (std::min)(
            static_cast<std::size_t>(kBlock),
            bytes.size() - block * static_cast<std::size_t>(kBlock));
        std::copy_n(first, valid_bytes, data.begin());
        // Byte equality with zero does not imply an AllZero record. A Data
        // snapshot can legitimately contain 64 KiB of zeroes and must reload
        // with Data semantics so the model does not erase record provenance.
        builder.data_block(block, data);
    }
    return builder.build();
}

std::unique_ptr<myrd::DynamicBlockStore> make_store_with_size(
    std::uint64_t logical_size_bytes,
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t generation,
    std::uint64_t low_blocks,
    std::uint64_t soft_blocks,
    std::uint64_t hard_blocks,
    HarnessContext& context) {
    auto created = myrd::DynamicBlockStore::create_for_testing(
        myrd::DynamicBlockStoreCreateArgs{
            logical_size_bytes,
            static_cast<std::uint32_t>(kSector),
            myrd::DynamicBlockStoreOptions{
                soft_blocks * kBlock,
                hard_blocks * kBlock,
                low_blocks * kBlock,
                4 * kBlock,
                kSnapshotRecordLimit,
                64,
            },
            myrd::InitialBlockState::BackingUnresident,
            std::move(backing),
            generation,
        },
        {});
    context.expect(created.ok(), "store construction failed");
    return created.move_value();
}

std::unique_ptr<myrd::DynamicBlockStore> make_store(
    std::uint64_t block_count,
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t generation,
    std::uint64_t low_blocks,
    std::uint64_t soft_blocks,
    std::uint64_t hard_blocks,
    HarnessContext& context) {
    return make_store_with_size(
        block_count * kBlock,
        std::move(backing),
        generation,
        low_blocks,
        soft_blocks,
        hard_blocks,
        context);
}

void observe_range_coverage(OperationCoverage& coverage,
                            const Range& range,
                            std::uint64_t logical_size_bytes) {
    if (range.offset / kBlock !=
        (range.offset + range.length - 1) / kBlock) {
        ++coverage.cross_block_ranges;
    }
    if (range.offset + range.length == logical_size_bytes &&
        range.length == kSector) {
        ++coverage.last_sector_ranges;
    }
    if (range.offset / kBlock == kMainBlockCount - 1) {
        ++coverage.repeated_hot_block_ranges;
    }
}

Range make_range(DeterministicRandom& random,
                 std::uint64_t operation_index,
                 std::uint64_t block_count,
                 bool allow_full_blocks) {
    const auto logical_size = block_count * kBlock;
    const auto mode = operation_index % 9;
    if (allow_full_blocks && mode == 0) {
        const auto block = random.next() % block_count;
        return {block * kBlock, kBlock};
    }
    if (allow_full_blocks && mode == 1 && block_count > 1) {
        const auto block = random.next() % (block_count - 1);
        return {block * kBlock, 2 * kBlock};
    }
    if (mode == 2) {
        const auto block = random.next() % block_count;
        return {block * kBlock, kSector};
    }
    if (mode == 3) {
        const auto block = random.next() % block_count;
        return {block * kBlock + kBlock - kSector, kSector};
    }
    if (mode == 4 && block_count > 1) {
        const auto block = random.next() % (block_count - 1);
        return {block * kBlock + kBlock - kSector, 2 * kSector};
    }
    if (mode == 5) {
        return {logical_size - kSector, kSector};
    }
    if (mode == 6) {
        return {(block_count - 1) * kBlock, kSector};
    }
    if (mode == 7 && (random.next() % 64) == 0 && block_count > 2) {
        const auto block = random.next() % (block_count - 2);
        return {block * kBlock, 2 * kBlock};
    }

    const auto total_sectors = logical_size / kSector;
    const auto first_sector = random.next() % total_sectors;
    const auto remaining = total_sectors - first_sector;
    const auto length_sectors = 1 + random.next() % (std::min)(8ull, remaining);
    return {first_sector * kSector, length_sectors * kSector};
}

std::string range_log(const char* type,
                      const Range& range,
                      std::uint64_t index) {
    std::ostringstream stream;
    stream << "index=" << index << " type=" << type
           << " offset=" << range.offset << " length=" << range.length;
    return stream.str();
}

void compare_snapshot(
    const std::vector<myrd::DirtyBlockSnapshot>& actual,
    const std::vector<myrd::reference::DirtyRecord>& expected,
    HarnessContext& context,
    const myrd::reference::BlockModel& model) {
    context.expect(actual.size() == expected.size(),
                   "dirty snapshot record count mismatch", &model);
    for (std::size_t index = 0; index < actual.size(); ++index) {
        const auto& left = actual[index];
        const auto& right = expected[index];
        context.expect(left.block_index == right.block_index &&
                           left.version == right.version &&
                           left.kind == to_native(right.kind) &&
                           left.base_generation == right.base_generation &&
                           left.data == right.data,
                       "dirty snapshot semantic mismatch", &model);
    }
}

std::vector<myrd::DirtyBlockSnapshot> collect_all_dirty(
    myrd::DynamicBlockStore& store,
    const myrd::reference::BlockModel& model,
    HarnessContext& context) {
    myrd::DirtyScanCursor native_cursor{};
    std::uint64_t model_cursor = 0;
    std::vector<myrd::DirtyBlockSnapshot> records;
    while (native_cursor.next_block_index < model.block_count()) {
        const auto before = native_cursor.next_block_index;
        auto expected = model.snapshot(
            model_cursor, 3 * static_cast<std::size_t>(kBlock),
            kSnapshotRecordLimit);
        auto actual = store.copy_dirty_batch(
            myrd::OperationContext{}, native_cursor, 3 * kBlock);
        context.expect(actual.ok(), "full dirty scan failed", &model);
        compare_snapshot(actual.value(), expected, context, model);
        context.expect(native_cursor.next_block_index == model_cursor,
                       "dirty scan cursor mismatch", &model);
        auto batch = actual.move_value();
        records.insert(records.end(),
                       std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
        context.expect(native_cursor.next_block_index > before ||
                           native_cursor.next_block_index ==
                               model.block_count(),
                       "dirty scan made no progress", &model);
    }
    context.expect(records.size() == model.dirty_blocks(),
                   "full dirty scan omitted model records", &model);
    return records;
}

void compare_gauges(myrd::DynamicBlockStore& store,
                     const myrd::reference::BlockModel& model,
                     HarnessContext& context) {
    const auto expected = model.gauges();
    const auto actual = store.stats();
    context.expect(
        store.backing_generation() == model.backing_generation() &&
            actual.backing_generation == model.backing_generation(),
        "backing generation gauge mismatch", &model);
    context.expect(actual.committed_bytes == expected.committed_blocks * kBlock,
                   "committed-byte gauge mismatch", &model);
    context.expect(actual.resident_clean_blocks ==
                           expected.resident_clean_blocks &&
                       actual.resident_dirty_blocks ==
                           expected.resident_dirty_blocks &&
                       actual.zero_clean_blocks == expected.zero_clean_blocks &&
                       actual.zero_dirty_blocks == expected.zero_dirty_blocks,
                   "stable-state gauge mismatch", &model);
    context.expect(actual.committed_bytes <= store.options().hard_limit_bytes,
                   "hard limit exceeded", &model);
    context.expect(
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(store) == 0,
        "commit reservation leaked at quiescent point", &model);
}

void compare_entries(myrd::DynamicBlockStore& store,
                     const myrd::reference::BlockModel& model,
                     HarnessContext& context) {
    for (std::uint64_t block = 0; block < model.block_count(); ++block) {
        myrd::BlockEntry actual{};
        context.expect(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                           store, block, actual),
                       "expected materialized metadata entry is absent", &model);
        const auto& expected = model.block(block);
        auto expected_state = myrd::BlockState::Unresident;
        if (expected.zero_state) {
            expected_state = expected.dirty
                ? myrd::BlockState::ZeroDirty
                : myrd::BlockState::ZeroClean;
        } else if (expected.resident) {
            expected_state = expected.dirty
                ? myrd::BlockState::ResidentDirty
                : myrd::BlockState::ResidentClean;
        }
        context.expect(actual.state == expected_state &&
                           actual.version == expected.version,
                       "block stable state/version mismatch", &model);
        if (expected.dirty) {
            context.expect(
                actual.dirty_base_generation ==
                        expected.dirty_base_generation &&
                    actual.dirty_kind == to_native(expected.dirty_kind),
                "dirty block metadata mismatch", &model);
        } else {
            context.expect(actual.clean_generation ==
                               expected.clean_generation,
                           "clean generation mismatch", &model);
        }
    }
}

void compare_full_disk(myrd::DynamicBlockStore& store,
                       myrd::reference::BlockModel& model,
                       HarnessContext& context) {
    std::vector<std::byte> actual(
        static_cast<std::size_t>(model.logical_size_bytes()));
    auto result = store.read(myrd::OperationContext{}, 0, actual);
    context.expect(result.ok(), "full-disk read failed", &model);
    const auto expected = model.read(0, model.logical_size_bytes());
    context.expect(actual == expected, "full-disk byte mismatch", &model);
    compare_gauges(store, model, context);
    compare_entries(store, model, context);
}

void compare_range_bytes(myrd::DynamicBlockStore& store,
                         myrd::reference::BlockModel& model,
                         const Range& range,
                         HarnessContext& context) {
    std::vector<std::byte> actual(static_cast<std::size_t>(range.length));
    const auto result = store.read(
        myrd::OperationContext{}, range.offset, actual);
    context.expect(result.ok(), "post-operation range read failed", &model);
    const auto expected = model.read(range.offset, range.length);
    context.expect(actual == expected,
                   "post-operation range byte mismatch", &model);
}

void expect_error_code(const myrd::Result<void>& result,
                       std::uint32_t expected_code,
                       const std::string& message,
                       HarnessContext& context,
                       const myrd::reference::BlockModel& model) {
    context.expect(!result.ok() &&
                       result.error().domain == myrd::ErrorDomain::storage &&
                       result.error().code == expected_code,
                   message, &model);
}

void write_both(myrd::DynamicBlockStore& store,
                myrd::reference::BlockModel& model,
                const Range& range,
                std::span<const std::byte> data,
                HarnessContext& context) {
    const auto actual = store.write(
        myrd::OperationContext{}, range.offset, data);
    context.expect(actual.ok(), "model write unexpectedly failed", &model);
    context.expect(model.write(range.offset, data),
                   "reference write rejected valid range", &model);
}

void checkpoint(myrd::DynamicBlockStore& store,
                myrd::reference::BlockModel& model,
                DeterministicRandom& random,
                HarnessContext& context,
                std::uint64_t operation_index) {
    const auto selected_block =
        (operation_index / 1024) % model.block_count();
    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=checkpoint_begin selected_block=" +
            std::to_string(selected_block));
    compare_full_disk(store, model, context);

    const Range selected{selected_block * kBlock, kSector};
    std::vector<std::byte> first_write(static_cast<std::size_t>(kSector));
    for (auto& value : first_write) {
        value = std::byte{static_cast<std::uint8_t>(random.next() & 0xFFu)};
    }
    write_both(store, model, selected, first_write, context);

    myrd::DirtyScanCursor old_native_cursor{selected_block};
    std::uint64_t old_model_cursor = selected_block;
    const auto old_expected = model.snapshot(
        old_model_cursor, static_cast<std::size_t>(kBlock),
        kSnapshotRecordLimit);
    auto old_actual_result = store.copy_dirty_batch(
        myrd::OperationContext{}, old_native_cursor, kBlock);
    context.expect(old_actual_result.ok(),
                   "old-version snapshot failed", &model);
    compare_snapshot(old_actual_result.value(), old_expected, context, model);
    context.expect(!old_actual_result.value().empty() &&
                       old_actual_result.value().front().block_index ==
                           selected_block,
                   "old-version snapshot did not capture selected block",
                   &model);
    const auto old_record = old_actual_result.value().front();
    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=checkpoint_snapshot block=" +
            std::to_string(old_record.block_index) + " version=" +
            std::to_string(old_record.version) + " kind=" +
            kind_name(to_reference(old_record.kind)) + " base_generation=" +
            std::to_string(old_record.base_generation));

    std::vector<std::byte> second_write(static_cast<std::size_t>(kSector));
    for (auto& value : second_write) {
        value = std::byte{static_cast<std::uint8_t>(random.next() & 0xFFu)};
    }
    write_both(store, model, selected, second_write, context);

    auto fresh_records = collect_all_dirty(store, model, context);
    context.expect(!fresh_records.empty(),
                   "checkpoint has no fresh dirty records", &model);
    const auto next_generation = model.backing_generation() + 1;

    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=checkpoint_ack_before_install block=" +
            std::to_string(fresh_records.front().block_index) +
            " version=" + std::to_string(fresh_records.front().version) +
            " base_generation=" +
            std::to_string(fresh_records.front().base_generation) +
            " ack_generation=" + std::to_string(next_generation));
    const auto before_install = store.mark_persisted(
        fresh_records.front().block_index,
        fresh_records.front().version,
        fresh_records.front().kind,
        next_generation);
    expect_error_code(
        before_install,
        static_cast<std::uint32_t>(
            myrd::DynamicBlockStoreErrorCode::BackingGenerationConflict),
        "ack-before-install returned the wrong result", context, model);
    context.expect(
        model.mark_persisted(
            fresh_records.front().block_index,
            fresh_records.front().version,
            to_reference(fresh_records.front().kind),
            next_generation) ==
            myrd::reference::PersistResult::BackingGenerationConflict,
        "reference returned the wrong ack-before-install result", &model);

    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=checkpoint_install generation=" +
            std::to_string(next_generation));
    auto successor = make_backing(
        model.visible_bytes(), next_generation ^ 0x5A5A'1234ull);
    const auto installed = store.install_backing_view(
        {next_generation, successor});
    context.expect(installed.ok(), "successor view install failed", &model);
    context.expect(model.install_backing(
                       next_generation, model.visible_bytes()),
                   "reference successor install failed", &model);
    compare_gauges(store, model, context);

    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=checkpoint_stale_version_ack block=" +
            std::to_string(old_record.block_index) + " version=" +
            std::to_string(old_record.version) + " ack_generation=" +
            std::to_string(next_generation));
    const auto stale_version = store.mark_persisted(
        old_record.block_index,
        old_record.version,
        old_record.kind,
        next_generation);
    expect_error_code(
        stale_version,
        static_cast<std::uint32_t>(
            myrd::OperationErrorCode::version_changed),
        "stale-version ack returned the wrong result", context, model);
    context.expect(
        model.mark_persisted(
            old_record.block_index,
            old_record.version,
            to_reference(old_record.kind),
            next_generation) ==
            myrd::reference::PersistResult::VersionChanged,
        "reference returned the wrong stale-version result", &model);

    const auto& first_fresh = fresh_records.front();
    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=checkpoint_stale_generation_ack block=" +
            std::to_string(first_fresh.block_index) + " version=" +
            std::to_string(first_fresh.version) + " base_generation=" +
            std::to_string(first_fresh.base_generation) +
            " ack_generation=" +
            std::to_string(first_fresh.base_generation));
    const auto stale_generation = store.mark_persisted(
        first_fresh.block_index,
        first_fresh.version,
        first_fresh.kind,
        first_fresh.base_generation);
    expect_error_code(
        stale_generation,
        static_cast<std::uint32_t>(
            myrd::DynamicBlockStoreErrorCode::BackingGenerationConflict),
        "stale-generation ack returned the wrong result", context, model);
    context.expect(
        model.mark_persisted(
            first_fresh.block_index,
            first_fresh.version,
            to_reference(first_fresh.kind),
            first_fresh.base_generation) ==
            myrd::reference::PersistResult::BackingGenerationConflict,
        "reference returned the wrong stale-generation result", &model);

    for (const auto& record : fresh_records) {
        context.set_operation(
            operation_index,
            "index=" + std::to_string(operation_index) +
                " type=checkpoint_exact_ack block=" +
                std::to_string(record.block_index) + " version=" +
                std::to_string(record.version) + " kind=" +
                kind_name(to_reference(record.kind)) +
                " base_generation=" +
                std::to_string(record.base_generation) +
                " ack_generation=" + std::to_string(next_generation));
        const auto native = store.mark_persisted(
            record.block_index,
            record.version,
            record.kind,
            next_generation);
        context.expect(native.ok(), "exact persisted ack failed", &model);
        const auto reference_kind = to_reference(record.kind);
        context.expect(model.mark_persisted(
                           record.block_index,
                           record.version,
                           reference_kind,
                           next_generation) ==
                           myrd::reference::PersistResult::Persisted,
                       "reference exact ack failed", &model);
    }
    context.expect(model.dirty_blocks() == 0,
                   "checkpoint left dirty model blocks", &model);

    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=checkpoint_evict generation=" +
            std::to_string(next_generation));
    const auto reclaimed =
        myrd::DynamicBlockStoreTestAccess::reclaim_clean_to(store, 0);
    context.expect(reclaimed.target_reached,
                   "clean eviction did not reach zero committed bytes", &model);
    model.evict_all_clean();
    compare_gauges(store, model, context);
    context.expect(store.stats().committed_bytes == 0,
                   "checkpoint eviction left committed bytes", &model);
    compare_entries(store, model, context);

    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=checkpoint_reload block=" +
            std::to_string(selected_block) + " generation=" +
            std::to_string(next_generation));
    std::vector<std::byte> reloaded(static_cast<std::size_t>(kBlock));
    const auto reload = store.read(
        myrd::OperationContext{}, selected_block * kBlock, reloaded);
    context.expect(reload.ok(), "post-ack reload failed", &model);
    const auto expected_reload = model.read(selected_block * kBlock, kBlock);
    context.expect(reloaded == expected_reload,
                   "post-ack eviction/reload resurrected old bytes", &model);

    std::ostringstream operation;
    operation << "index=" << operation_index
              << " type=checkpoint installed_generation=" << next_generation
              << " ack_count=" << fresh_records.size()
              << " ack_generation=" << next_generation
              << " stale_block=" << old_record.block_index
              << " stale_version=" << old_record.version
              << " stale_kind="
              << kind_name(to_reference(old_record.kind))
              << " stale_base_generation=" << old_record.base_generation
              << " reload_block=" << selected_block;
    context.set_operation(operation_index, operation.str());
}

std::uint64_t digest_model(const myrd::reference::BlockModel& model) {
    std::uint64_t hash = 1'469'598'103'934'665'603ull;
    const auto mix = [&hash](std::uint64_t value) {
        for (std::uint32_t shift = 0; shift < 64; shift += 8) {
            hash ^= (value >> shift) & 0xFFu;
            hash *= 1'099'511'628'211ull;
        }
    };
    for (const auto value : model.visible_bytes()) {
        hash ^= std::to_integer<std::uint8_t>(value);
        hash *= 1'099'511'628'211ull;
    }
    mix(model.backing_generation());
    for (std::uint64_t block = 0; block < model.block_count(); ++block) {
        const auto& state = model.block(block);
        mix(state.version);
        mix(state.dirty_base_generation);
        mix(state.clean_generation);
        mix(static_cast<std::uint64_t>(state.dirty_kind));
        mix(state.dirty ? 1 : 0);
        mix(state.resident ? 1 : 0);
        mix(state.zero_state ? 1 : 0);
    }
    return hash;
}

std::optional<std::uint64_t> diagnostic_seed_override() {
    char* raw_text = nullptr;
    std::size_t text_size = 0;
    if (_dupenv_s(&raw_text, &text_size, "MYRD_M2_MODEL_SEED") != 0) {
        std::cerr << "MYRD_M2_MODEL_SEED could not be read" << '\n';
        std::_Exit(1);
    }
    const std::unique_ptr<char, decltype(&std::free)> override_text(
        raw_text, &std::free);
    if (text_size == 0 || override_text == nullptr ||
        *override_text == '\0') {
        return std::nullopt;
    }
    const char* first_non_space = override_text.get();
    while (*first_non_space == ' ' || *first_non_space == '\t' ||
           *first_non_space == '\r' || *first_non_space == '\n' ||
           *first_non_space == '\f' || *first_non_space == '\v') {
        ++first_non_space;
    }
    const bool negative = *first_non_space == '-';
    errno = 0;
    char* end = nullptr;
    const auto parsed = std::strtoull(override_text.get(), &end, 0);
    if (negative || end == override_text.get() || *end != '\0' ||
        errno == ERANGE) {
        std::cerr << "MYRD_M2_MODEL_SEED must be a complete uint64 integer"
                  << '\n';
        std::_Exit(1);
    }
    return parsed;
}

void run_hard_pressure_probe(HarnessContext& context,
                             std::uint64_t operation_index) {
    constexpr std::uint64_t block_count = 5;
    constexpr std::uint64_t seed = 0x5052'4553'5355'5245ull;
    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=hard_pressure_setup hard_blocks=4 dirty_blocks=4");
    auto initial = make_initial_bytes(block_count, seed);
    auto backing = make_backing(initial, seed);
    auto store = make_store(block_count, backing, 3, 1, 3, 4, context);
    myrd::reference::BlockModel model(
        block_count * kBlock, std::move(initial), 3);
    const auto worker_passes_before =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(*store);

    for (std::uint64_t block = 0; block < 4; ++block) {
        std::vector<std::byte> data(
            static_cast<std::size_t>(kBlock),
            std::byte{static_cast<std::uint8_t>(0x40 + block)});
        const Range range{block * kBlock, kBlock};
        write_both(*store, model, range, data, context);
    }
    context.expect(
        myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
            *store,
            worker_passes_before + 1,
            std::chrono::steady_clock::now() + std::chrono::seconds(2)),
        "dirty-only pressure worker did not become idle", &model);

    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=hard_pressure_read_through block=4 generation=3");
    std::vector<std::byte> actual(static_cast<std::size_t>(kBlock));
    const auto read = store->read(
        myrd::OperationContext{}, 4 * kBlock, actual);
    context.expect(read.ok(), "hard-pressure read-through failed", &model);
    const auto expected = model.read(4 * kBlock, kBlock, false);
    context.expect(actual == expected,
                   "hard-pressure read-through byte mismatch", &model);
    myrd::BlockEntry entry{};
    context.expect(myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                       *store, 4, entry),
                   "read-through metadata entry missing", &model);
    context.expect(entry.state == myrd::BlockState::Unresident &&
                       store->stats().committed_bytes == 4 * kBlock &&
                       store->stats().uncached_reads_total == 1,
                   "read-through incorrectly cached or changed accounting",
                   &model);

    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=hard_pressure_rejected_write block=4 generation=3");
    const auto rejections_before_write =
        store->stats().hard_limit_rejections;
    std::vector<std::byte> replacement(
        static_cast<std::size_t>(kBlock), std::byte{0xE3});
    const auto rejected = store->write(
        myrd::OperationContext{}, 4 * kBlock, replacement);
    expect_error_code(
        rejected,
        static_cast<std::uint32_t>(
            myrd::OperationErrorCode::memory_hard_limit),
        "hard-pressure mutation returned the wrong result", context, model);
    const auto rejections_after_write = store->stats().hard_limit_rejections;
    context.expect(rejections_after_write > rejections_before_write,
                   "rejected pressure write did not advance accounting",
                   &model);
    std::fill(actual.begin(), actual.end(), std::byte{0});
    context.expect(store->read(
                       myrd::OperationContext{}, 4 * kBlock, actual).ok(),
                   "post-rejection read-through failed", &model);
    context.expect(actual == expected,
                   "rejected mutation changed visible bytes", &model);

    const Range resident_range{0, 4 * kBlock};
    compare_range_bytes(*store, model, resident_range, context);
    collect_all_dirty(*store, model, context);
    compare_gauges(*store, model, context);
    compare_entries(*store, model, context);
    context.expect(
        store->stats().hard_limit_rejections >= rejections_after_write,
        "pressure hard-limit rejection accounting regressed", &model);
    context.expect(store->validate_invariants_for_test().ok(),
                   "pressure scenario invariant mismatch", &model);
}

void run_sparse_distant_probe(HarnessContext& context,
                              std::uint64_t operation_index) {
    constexpr std::uint64_t block_count = 1025;
    constexpr std::uint64_t distant_block = 1024;
    constexpr std::uint64_t seed = 0x5350'4152'5345'4C46ull;
    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=sparse_distant_probe blocks=0,1024");

    myrd::test::FakeBackingStore::BlockData first{};
    myrd::test::FakeBackingStore::BlockData distant{};
    for (std::size_t offset = 0; offset < first.size(); ++offset) {
        first[offset] = std::byte{
            static_cast<std::uint8_t>((offset * 11 + 0x21) & 0xFFu)};
        distant[offset] = std::byte{
            static_cast<std::uint8_t>((offset * 29 + 0x7D) & 0xFFu)};
    }
    myrd::test::FakeBackingStoreBuilder builder(seed);
    auto backing = builder.data_block(0, first)
                       .data_block(distant_block, distant)
                       .build();
    auto store = make_store(block_count, backing, 3, 1, 3, 4, context);

    std::vector<std::byte> first_patch(
        static_cast<std::size_t>(kSector), std::byte{0xA1});
    std::vector<std::byte> distant_patch(
        static_cast<std::size_t>(kSector), std::byte{0xD4});
    context.expect(store->write(
                       myrd::OperationContext{}, 0, first_patch).ok(),
                   "sparse first-leaf write failed");
    context.expect(store->write(
                       myrd::OperationContext{}, distant_block * kBlock,
                       distant_patch).ok(),
                   "sparse distant-leaf write failed");
    std::copy(first_patch.begin(), first_patch.end(), first.begin());
    std::copy(distant_patch.begin(), distant_patch.end(), distant.begin());

    std::vector<std::byte> actual(static_cast<std::size_t>(kBlock));
    context.expect(store->read(
                       myrd::OperationContext{}, 0, actual).ok() &&
                       std::equal(actual.begin(), actual.end(), first.begin()),
                   "sparse first-leaf byte mismatch");
    context.expect(store->read(
                       myrd::OperationContext{}, distant_block * kBlock,
                       actual).ok() &&
                       std::equal(actual.begin(), actual.end(), distant.begin()),
                   "sparse distant-leaf byte mismatch");

    myrd::DirtyScanCursor cursor{};
    auto snapshots = store->copy_dirty_batch(
        myrd::OperationContext{}, cursor, 2 * kBlock);
    context.expect(snapshots.ok() && snapshots.value().size() == 2 &&
                       snapshots.value()[0].block_index == 0 &&
                       snapshots.value()[0].data ==
                           std::vector<std::byte>(first.begin(), first.end()) &&
                       snapshots.value()[1].block_index == distant_block &&
                       snapshots.value()[1].data ==
                           std::vector<std::byte>(distant.begin(), distant.end()) &&
                       cursor.next_block_index == block_count,
                   "sparse cross-leaf dirty scan mismatch");
    myrd::BlockEntry first_entry{};
    myrd::BlockEntry distant_entry{};
    context.expect(
        myrd::DynamicBlockStoreTestAccess::snapshot_entry(
            *store, 0, first_entry) &&
            myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                *store, distant_block, distant_entry) &&
            first_entry.state == myrd::BlockState::ResidentDirty &&
            distant_entry.state == myrd::BlockState::ResidentDirty &&
            store->stats().committed_bytes == 2 * kBlock &&
            store->validate_invariants_for_test().ok(),
        "sparse cross-leaf metadata/accounting mismatch");
}

void run_partial_final_block_probe(HarnessContext& context,
                                   std::uint64_t operation_index) {
    constexpr std::uint64_t logical_size = kBlock + kSector;
    constexpr std::uint64_t seed = 0x5041'5254'4941'4C42ull;
    context.set_operation(
        operation_index,
        "index=" + std::to_string(operation_index) +
            " type=partial_final_block logical_bytes=" +
            std::to_string(logical_size));
    std::vector<std::byte> initial(static_cast<std::size_t>(logical_size));
    for (std::size_t offset = 0; offset < initial.size(); ++offset) {
        initial[offset] = std::byte{
            static_cast<std::uint8_t>((offset * 7 + 0x31) & 0xFFu)};
    }
    auto backing = make_backing(initial, seed);
    auto store = make_store_with_size(
        logical_size, backing, 3, 1, 2, 3, context);
    myrd::reference::BlockModel model(logical_size, std::move(initial), 3);

    const Range final_sector{kBlock, kSector};
    std::vector<std::byte> replacement(
        static_cast<std::size_t>(kSector), std::byte{0xC7});
    write_both(*store, model, final_sector, replacement, context);
    compare_range_bytes(*store, model, final_sector, context);
    const auto records = collect_all_dirty(*store, model, context);
    context.expect(records.size() == 1 && records.front().block_index == 1 &&
                       records.front().data.size() == kBlock &&
                       std::all_of(
                           records.front().data.begin() +
                               static_cast<std::ptrdiff_t>(kSector),
                           records.front().data.end(),
                           [](std::byte value) {
                               return value == std::byte{0};
                           }),
                   "partial final block snapshot was not zero padded", &model);
    compare_full_disk(*store, model, context);
}

RunReport run_reference_model(std::uint64_t seed) {
    HarnessContext context(seed);
    context.set_operation(0, "index=0 type=construct");
    auto initial = make_initial_bytes(kMainBlockCount, seed);
    auto backing = make_backing(initial, seed ^ 0xA11C'E55ull);
    auto store = make_store(
        kMainBlockCount, backing, 3, 8, kMainBlockCount,
        kMainBlockCount + 1, context);
    myrd::reference::BlockModel model(
        kMainBlockCount * kBlock, std::move(initial), 3);
    DeterministicRandom random(seed);
    myrd::DirtyScanCursor native_cursor{};
    std::uint64_t model_cursor = 0;
    std::uint64_t completed = 0;
    OperationCoverage coverage{};
    const auto started = std::chrono::steady_clock::now();

    for (std::uint64_t index = 0; index < kOperationCount; ++index) {
        if (index == 12'345) {
            run_hard_pressure_probe(context, index);
            ++coverage.pressure_probes;
        } else if (index == 54'321) {
            run_sparse_distant_probe(context, index);
            ++coverage.sparse_distant_probes;
        } else if ((index != 0 && index % 1024 == 1023) ||
            index + 1 == kOperationCount) {
            checkpoint(*store, model, random, context, index);
            ++coverage.checkpoints;
        } else {
            const auto choice = random.next() % 100;
            if (choice < 35) {
                const auto range = make_range(
                    random, index, model.block_count(), false);
                observe_range_coverage(
                    coverage, range, model.logical_size_bytes());
                context.set_operation(index, range_log("read", range, index));
                std::vector<std::byte> actual(
                    static_cast<std::size_t>(range.length));
                const auto result = store->read(
                    myrd::OperationContext{}, range.offset, actual);
                context.expect(result.ok(), "random read failed", &model);
                const auto expected = model.read(range.offset, range.length);
                context.expect(actual == expected,
                               "random read byte mismatch", &model);
                ++coverage.reads;
            } else if (choice < 65) {
                const auto range = make_range(
                    random, index, model.block_count(), false);
                observe_range_coverage(
                    coverage, range, model.logical_size_bytes());
                context.set_operation(index, range_log("write", range, index));
                std::vector<std::byte> data(
                    static_cast<std::size_t>(range.length));
                for (auto& value : data) {
                    value = std::byte{
                        static_cast<std::uint8_t>(random.next() & 0xFFu)};
                }
                write_both(*store, model, range, data, context);
                compare_range_bytes(*store, model, range, context);
                ++coverage.writes;
            } else if (choice < 80) {
                const auto range = make_range(
                    random, index, model.block_count(), true);
                observe_range_coverage(
                    coverage, range, model.logical_size_bytes());
                if ((range.offset % kBlock) == 0 &&
                    (range.length % kBlock) == 0) {
                    ++coverage.full_zeroes;
                } else {
                    ++coverage.partial_zeroes;
                }
                context.set_operation(index, range_log("zero", range, index));
                const myrd::DataRange native_range{range.offset, range.length};
                const auto result = store->zero(
                    myrd::OperationContext{},
                    std::span<const myrd::DataRange>(&native_range, 1));
                context.expect(result.ok(), "random ZERO failed", &model);
                context.expect(model.zero(
                                   range.offset,
                                   range.length,
                                   myrd::reference::DirtyKind::Zero),
                               "reference ZERO failed", &model);
                compare_range_bytes(*store, model, range, context);
                ++coverage.zeroes;
            } else if (choice < 95) {
                const auto range = make_range(
                    random, index, model.block_count(), true);
                observe_range_coverage(
                    coverage, range, model.logical_size_bytes());
                if ((range.offset % kBlock) == 0 &&
                    (range.length % kBlock) == 0) {
                    ++coverage.full_unmaps;
                } else {
                    ++coverage.partial_unmaps;
                }
                context.set_operation(index, range_log("unmap", range, index));
                const myrd::DataRange native_range{range.offset, range.length};
                const auto result = store->unmap(
                    myrd::OperationContext{},
                    std::span<const myrd::DataRange>(&native_range, 1));
                context.expect(result.ok(), "random UNMAP failed", &model);
                context.expect(model.zero(
                                   range.offset,
                                   range.length,
                                   myrd::reference::DirtyKind::Discard),
                               "reference UNMAP failed", &model);
                compare_range_bytes(*store, model, range, context);
                ++coverage.unmaps;
            } else if (choice < 98) {
                if (native_cursor.next_block_index == model.block_count()) {
                    const auto restart = random.next() % model.block_count();
                    native_cursor.next_block_index = restart;
                    model_cursor = restart;
                }
                const auto payload_blocks = 1 + random.next() % 4;
                std::ostringstream operation;
                operation << "index=" << index << " type=dirty_scan cursor="
                          << model_cursor << " payload_blocks="
                          << payload_blocks;
                context.set_operation(index, operation.str());
                const auto expected = model.snapshot(
                    model_cursor,
                    static_cast<std::size_t>(payload_blocks * kBlock),
                    kSnapshotRecordLimit);
                auto actual = store->copy_dirty_batch(
                    myrd::OperationContext{}, native_cursor,
                    payload_blocks * kBlock);
                context.expect(actual.ok(), "random dirty scan failed", &model);
                compare_snapshot(actual.value(), expected, context, model);
                context.expect(native_cursor.next_block_index == model_cursor,
                               "random dirty cursor mismatch", &model);
                if (!actual.value().empty()) {
                    const auto& first = actual.value().front();
                    operation << " first_block=" << first.block_index
                              << " first_version=" << first.version
                              << " first_kind="
                              << kind_name(to_reference(first.kind))
                              << " first_base_generation="
                              << first.base_generation;
                    context.set_operation(index, operation.str());
                }
                ++coverage.dirty_scans;
            } else if (choice < 99) {
                auto records = collect_all_dirty(*store, model, context);
                if (records.empty()) {
                    const Range setup{0, kSector};
                    std::vector<std::byte> data(
                        static_cast<std::size_t>(kSector), std::byte{0x6B});
                    write_both(*store, model, setup, data, context);
                    compare_range_bytes(*store, model, setup, context);
                    records = collect_all_dirty(*store, model, context);
                }
                context.expect(!records.empty(),
                               "ack probe could not produce a dirty record",
                               &model);
                const auto& record = records.front();
                const auto invalid_generation = (index & 1) == 0
                    ? model.backing_generation() + 1
                    : record.base_generation;
                std::ostringstream operation;
                operation << "index=" << index
                          << " type=rejected_ack block="
                          << record.block_index << " version="
                          << record.version << " kind="
                          << kind_name(to_reference(record.kind))
                          << " base_generation=" << record.base_generation
                          << " ack_generation=" << invalid_generation;
                context.set_operation(index, operation.str());
                const auto rejected = store->mark_persisted(
                    record.block_index,
                    record.version,
                    record.kind,
                    invalid_generation);
                expect_error_code(
                    rejected,
                    static_cast<std::uint32_t>(
                        myrd::DynamicBlockStoreErrorCode::
                            BackingGenerationConflict),
                    "invalid acknowledgement returned the wrong result",
                    context, model);
                context.expect(
                    model.mark_persisted(
                        record.block_index,
                        record.version,
                        to_reference(record.kind),
                        invalid_generation) ==
                        myrd::reference::PersistResult::
                            BackingGenerationConflict,
                    "reference invalid-ack result mismatch", &model);
                ++coverage.rejected_acks;
            } else {
                context.set_operation(
                    index, "index=" + std::to_string(index) +
                               " type=zero_length_rejection");
                std::vector<std::byte> empty;
                const auto read = store->read(
                    myrd::OperationContext{}, 0, empty);
                const auto write = store->write(
                    myrd::OperationContext{}, 0, empty);
                context.expect(!read.ok() && !write.ok() &&
                                   !model.valid_io(0, 0),
                               "zero-length READ/WRITE was accepted", &model);
                ++coverage.zero_length_rejections;
            }
        }

        ++completed;
        if ((index + 1) % 257 == 0) {
            context.expect(model.validate(),
                           "reference invariant failed", &model);
            context.expect(store->validate_invariants_for_test().ok(),
                           "store invariant failed", &model);
            compare_gauges(*store, model, context);
        }
        if ((index + 1) % 4096 == 2048) {
            compare_full_disk(*store, model, context);
        }
    }

    compare_full_disk(*store, model, context);
    collect_all_dirty(*store, model, context);
    context.expect(model.validate(), "final reference invariant failed", &model);
    context.expect(store->validate_invariants_for_test().ok(),
                   "final store invariant failed", &model);
    context.expect(coverage.total() == kOperationCount &&
                       coverage.reads != 0 && coverage.writes != 0 &&
                       coverage.zeroes != 0 && coverage.unmaps != 0 &&
                       coverage.dirty_scans != 0 &&
                       coverage.rejected_acks != 0 &&
                       coverage.zero_length_rejections != 0 &&
                       coverage.checkpoints != 0 &&
                       coverage.pressure_probes == 1 &&
                       coverage.sparse_distant_probes == 1 &&
                       coverage.full_zeroes != 0 &&
                       coverage.partial_zeroes != 0 &&
                       coverage.full_unmaps != 0 &&
                       coverage.partial_unmaps != 0 &&
                       coverage.cross_block_ranges != 0 &&
                       coverage.last_sector_ranges != 0 &&
                       coverage.repeated_hot_block_ranges > 1,
                   "operation mix did not cover every required class", &model);
    const auto digest = digest_model(model);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::printf(
        "M2_MODEL completed_operations=%llu seed=0x%016llX digest=0x%016llX elapsed_ms=%lld\n",
        static_cast<unsigned long long>(completed),
        static_cast<unsigned long long>(seed),
        static_cast<unsigned long long>(digest),
        static_cast<long long>(elapsed));
    std::printf(
        "M2_MODEL_COVERAGE reads=%llu writes=%llu zero=%llu unmap=%llu dirty_scans=%llu rejected_acks=%llu zero_length=%llu checkpoints=%llu pressure=%llu sparse_distant=%llu full_zero=%llu partial_zero=%llu full_unmap=%llu partial_unmap=%llu cross_block=%llu last_sector=%llu repeated_hot_block=%llu\n",
        static_cast<unsigned long long>(coverage.reads),
        static_cast<unsigned long long>(coverage.writes),
        static_cast<unsigned long long>(coverage.zeroes),
        static_cast<unsigned long long>(coverage.unmaps),
        static_cast<unsigned long long>(coverage.dirty_scans),
        static_cast<unsigned long long>(coverage.rejected_acks),
        static_cast<unsigned long long>(coverage.zero_length_rejections),
        static_cast<unsigned long long>(coverage.checkpoints),
        static_cast<unsigned long long>(coverage.pressure_probes),
        static_cast<unsigned long long>(coverage.sparse_distant_probes),
        static_cast<unsigned long long>(coverage.full_zeroes),
        static_cast<unsigned long long>(coverage.partial_zeroes),
        static_cast<unsigned long long>(coverage.full_unmaps),
        static_cast<unsigned long long>(coverage.partial_unmaps),
        static_cast<unsigned long long>(coverage.cross_block_ranges),
        static_cast<unsigned long long>(coverage.last_sector_ranges),
        static_cast<unsigned long long>(coverage.repeated_hot_block_ranges));
    return {seed, completed, digest};
}

} // namespace

TEST_CASE(m2_reference_model_hard_pressure_preserves_bytes) {
    constexpr std::uint64_t seed = 0x5052'4553'5355'5245ull;
    HarnessContext context(seed);
    run_hard_pressure_probe(context, 0);
}

TEST_CASE(m2_reference_model_handles_partial_final_block) {
    constexpr std::uint64_t seed = 0x5041'5254'4941'4C42ull;
    HarnessContext context(seed);
    run_partial_final_block_probe(context, 0);
}

TEST_CASE(m2_reference_model_runs_exactly_100000_operations) {
    const auto report = run_reference_model(kCanonicalSeed);
    REQUIRE_EQ(report.seed, kCanonicalSeed);
    REQUIRE_EQ(report.completed_operations, kOperationCount);
    REQUIRE(report.digest != 0);
}

TEST_CASE(m2_reference_model_accepts_optional_diagnostic_seed) {
    const auto override_seed = diagnostic_seed_override();
    if (!override_seed || *override_seed == kCanonicalSeed) {
        return;
    }
    const auto report = run_reference_model(*override_seed);
    REQUIRE_EQ(report.seed, *override_seed);
    REQUIRE_EQ(report.completed_operations, kOperationCount);
    REQUIRE(report.digest != 0);
}
