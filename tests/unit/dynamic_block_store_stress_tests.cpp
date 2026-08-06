#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "test_framework.h"

#include "fake_backing_store.h"
#include "src/storage/dynamic_block_store.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/eviction_manager.h"
#include "src/storage/virtual_memory_arena.h"

#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

constexpr std::uint64_t kBlock = myrd::kBlockSizeBytes;
constexpr std::uint64_t kSector = 512;
constexpr std::uint64_t kStressSeed = 0x4D59'5244'5354'5231ull;
constexpr std::uint64_t kStressBlockCount = 2050;
constexpr std::uint32_t kStressRounds = 3;
constexpr std::uint32_t kOperationsPerRole = 192;
constexpr auto kOperationBound = std::chrono::seconds(5);
constexpr auto kCheckpointBound = std::chrono::seconds(5);

constexpr std::array<std::uint64_t, 20> kSelectedBlocks{
    0, 1, 2, 3, 4, 61, 62, 63, 64, 65,
    1022, 1023, 1024, 1025, 1026,
    2045, 2046, 2047, 2048, 2049,
};

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
    std::uint64_t state_{};
};

myrd::OperationContext bounded_context() {
    return myrd::OperationContext{
        {}, std::chrono::steady_clock::now() + kOperationBound};
}

std::uint64_t block_digest(std::span<const std::byte> bytes) noexcept {
    std::uint64_t digest = 0xCBF2'9CE4'8422'2325ull;
    for (const auto value : bytes) {
        digest ^= std::to_integer<std::uint8_t>(value);
        digest *= 0x0000'0100'0000'01B3ull;
    }
    return digest;
}

myrd::test::FakeBackingStore::BlockData make_write_data(
    std::uint64_t key) {
    myrd::test::FakeBackingStore::BlockData data{};
    DeterministicRandom random(key);
    for (auto& value : data) {
        value = std::byte{static_cast<std::uint8_t>(random.next() & 0xFFu)};
    }
    return data;
}

class FailureState final {
public:
    void note(std::string role,
              std::uint32_t round,
              std::uint64_t operation,
              std::string detail) {
        std::ostringstream stream;
        stream << "role=" << role << " round=" << round
               << " operation=" << operation << " " << detail;
        std::scoped_lock lock(mutex_);
        history_.push_back(stream.str());
        if (history_.size() > 16) {
            history_.pop_front();
        }
    }

    void record(std::string role,
                std::uint32_t round,
                std::uint64_t operation,
                std::string message) {
        bool expected = false;
        if (!failed_.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel)) {
            return;
        }
        std::ostringstream stream;
        stream << "M2_STRESS_FAILURE seed=0x" << std::hex << kStressSeed
               << std::dec << " role=" << role << " round=" << round
               << " operation=" << operation << " message=" << message;
        std::scoped_lock lock(mutex_);
        message_ = stream.str();
    }

    bool failed() const noexcept {
        return failed_.load(std::memory_order_acquire);
    }

    std::string message() const {
        std::scoped_lock lock(mutex_);
        std::ostringstream stream;
        stream << message_;
        for (const auto& item : history_) {
            stream << "\nM2_STRESS_HISTORY " << item;
        }
        return stream.str();
    }

private:
    std::atomic<bool> failed_{};
    mutable std::mutex mutex_;
    std::string message_;
    std::deque<std::string> history_;
};

struct ShadowHistory {
    std::uint64_t version{};
    myrd::DirtyRecordKind kind{myrd::DirtyRecordKind::Data};
    std::uint64_t digest{};
    myrd::BackingGeneration dirty_base_min{};
    myrd::BackingGeneration dirty_base_max{};
};

struct ShadowBlock {
    explicit ShadowBlock(std::uint64_t index) : block_index(index) {
        bytes.fill(std::byte{0});
        history.push_back({0, myrd::DirtyRecordKind::Data,
                           block_digest(bytes), 1, 1});
    }

    std::uint64_t block_index{};
    std::mutex operation_mutex;
    std::mutex mutex;
    std::condition_variable condition;
    bool mutation_in_progress{};
    myrd::test::FakeBackingStore::BlockData bytes{};
    std::uint64_t version{};
    bool dirty{};
    myrd::DirtyRecordKind dirty_kind{myrd::DirtyRecordKind::Data};
    myrd::BackingGeneration dirty_base_min{1};
    myrd::BackingGeneration dirty_base_max{1};
    myrd::BackingGeneration clean_generation{1};
    std::vector<ShadowHistory> history;
};

class ShadowModel final {
public:
    ShadowModel() {
        blocks_.reserve(kSelectedBlocks.size());
        for (const auto block : kSelectedBlocks) {
            blocks_.push_back(std::make_unique<ShadowBlock>(block));
        }
    }

    ShadowBlock& by_slot(std::size_t slot) noexcept {
        return *blocks_[slot % blocks_.size()];
    }

    ShadowBlock* find(std::uint64_t block_index) noexcept {
        for (auto& block : blocks_) {
            if (block->block_index == block_index) {
                return block.get();
            }
        }
        return nullptr;
    }

    const std::vector<std::unique_ptr<ShadowBlock>>& blocks() const noexcept {
        return blocks_;
    }

private:
    std::vector<std::unique_ptr<ShadowBlock>> blocks_;
};

void record_shadow_mutation(
    ShadowBlock& shadow,
    myrd::DirtyRecordKind kind,
    const myrd::test::FakeBackingStore::BlockData& bytes,
    myrd::BackingGeneration dirty_base_min,
    myrd::BackingGeneration dirty_base_max) {
    shadow.bytes = bytes;
    ++shadow.version;
    shadow.dirty = true;
    shadow.dirty_kind = kind;
    shadow.dirty_base_min = dirty_base_min;
    shadow.dirty_base_max = dirty_base_max;
    shadow.history.push_back(
        {shadow.version, kind, block_digest(shadow.bytes),
         dirty_base_min, dirty_base_max});
}

class ShadowMutationGuard final {
public:
    explicit ShadowMutationGuard(ShadowBlock& shadow)
        : shadow_(shadow), operation_lock_(shadow.operation_mutex) {
        std::scoped_lock lock(shadow_.mutex);
        shadow_.mutation_in_progress = true;
    }

    ~ShadowMutationGuard() noexcept {
        if (active_) {
            clear_noexcept();
        }
    }

    ShadowMutationGuard(const ShadowMutationGuard&) = delete;
    ShadowMutationGuard& operator=(const ShadowMutationGuard&) = delete;

    void finish_without_change() noexcept {
        clear_noexcept();
    }

    void finish_success(
        myrd::DirtyRecordKind kind,
        const myrd::test::FakeBackingStore::BlockData& bytes,
        myrd::BackingGeneration dirty_base_min,
        myrd::BackingGeneration dirty_base_max) {
        {
            std::scoped_lock lock(shadow_.mutex);
            record_shadow_mutation(
                shadow_, kind, bytes, dirty_base_min, dirty_base_max);
            shadow_.mutation_in_progress = false;
        }
        active_ = false;
        shadow_.condition.notify_all();
    }

private:
    void clear_noexcept() noexcept {
        try {
            {
                std::scoped_lock lock(shadow_.mutex);
                shadow_.mutation_in_progress = false;
            }
            active_ = false;
            shadow_.condition.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    ShadowBlock& shadow_;
    std::unique_lock<std::mutex> operation_lock_;
    bool active_{true};
};

std::string error_code(const myrd::Result<void>& result) {
    if (result.ok()) {
        return "ok";
    }
    return std::to_string(result.error().code);
}

class TrackingVirtualMemoryApi final : public myrd::IVirtualMemoryApi {
public:
    void* allocate(void* address,
                   std::size_t size_bytes,
                   std::uint32_t allocation_type,
                   std::uint32_t protection) noexcept override {
        auto* result = ::VirtualAlloc(
            address, size_bytes, allocation_type, protection);
        const auto native_error = result == nullptr
            ? ::GetLastError()
            : ERROR_SUCCESS;
        if (result != nullptr) {
            std::scoped_lock lock(mutex_);
            if ((allocation_type & MEM_RESERVE) != 0) {
                reservations_[result] = size_bytes;
                ++reserve_calls_;
            }
            if ((allocation_type & MEM_COMMIT) != 0) {
                committed_.insert(result);
            }
        }
        last_error_.store(native_error, std::memory_order_release);
        return result;
    }

    bool free(void* address,
              std::size_t size_bytes,
              std::uint32_t free_type) noexcept override {
        const bool ok =
            ::VirtualFree(address, size_bytes, free_type) != FALSE;
        const auto native_error = ok ? ERROR_SUCCESS : ::GetLastError();
        if (ok) {
            std::scoped_lock lock(mutex_);
            if (free_type == MEM_DECOMMIT) {
                committed_.erase(address);
                ++decommit_calls_;
            } else if (free_type == MEM_RELEASE) {
                const auto reservation = reservations_.find(address);
                if (reservation != reservations_.end()) {
                    const auto first = reinterpret_cast<std::uintptr_t>(
                        reservation->first);
                    const auto last = first + reservation->second;
                    for (auto committed = committed_.begin();
                         committed != committed_.end();) {
                        const auto current = reinterpret_cast<std::uintptr_t>(
                            *committed);
                        if (current >= first && current < last) {
                            committed = committed_.erase(committed);
                        } else {
                            ++committed;
                        }
                    }
                    reservations_.erase(reservation);
                }
                ++release_calls_;
            }
        }
        last_error_.store(native_error, std::memory_order_release);
        return ok;
    }

    bool query(const void* address,
               myrd::VirtualMemoryRegionInfo& information) noexcept override {
        MEMORY_BASIC_INFORMATION native{};
        if (::VirtualQuery(address, &native, sizeof(native)) == 0) {
            last_error_.store(::GetLastError(), std::memory_order_release);
            return false;
        }
        information.allocation_base = native.AllocationBase;
        information.base_address = native.BaseAddress;
        information.region_size_bytes = native.RegionSize;
        information.state = native.State == MEM_COMMIT
            ? myrd::VirtualMemoryRegionState::Committed
            : (native.State == MEM_RESERVE
                ? myrd::VirtualMemoryRegionState::Reserved
                : (native.State == MEM_FREE
                    ? myrd::VirtualMemoryRegionState::Free
                    : myrd::VirtualMemoryRegionState::Unknown));
        last_error_.store(ERROR_SUCCESS, std::memory_order_release);
        return true;
    }

    std::uint32_t last_error() const noexcept override {
        return last_error_.load(std::memory_order_acquire);
    }

    std::uint64_t active_reservations() const {
        std::scoped_lock lock(mutex_);
        return reservations_.size();
    }

    std::uint64_t active_committed_bytes() const {
        std::scoped_lock lock(mutex_);
        return committed_.size() * kBlock;
    }

    std::uint64_t reserve_calls() const {
        std::scoped_lock lock(mutex_);
        return reserve_calls_;
    }

    std::uint64_t release_calls() const {
        std::scoped_lock lock(mutex_);
        return release_calls_;
    }

    std::uint64_t decommit_calls() const {
        std::scoped_lock lock(mutex_);
        return decommit_calls_;
    }

private:
    mutable std::mutex mutex_;
    std::unordered_map<void*, std::size_t> reservations_;
    std::unordered_set<void*> committed_;
    std::uint64_t reserve_calls_{};
    std::uint64_t release_calls_{};
    std::uint64_t decommit_calls_{};
    std::atomic<std::uint32_t> last_error_{ERROR_SUCCESS};
};

std::shared_ptr<const myrd::test::FakeBackingStore> make_zero_backing(
    std::uint64_t block_count,
    std::uint64_t seed,
    const std::shared_ptr<myrd::test::FakeBackingStoreLifetimeTracker>&
        lifetime = {}) {
    myrd::test::FakeBackingStoreBuilder builder(seed);
    for (std::uint64_t block = 0; block < block_count; ++block) {
        builder.all_zero_block(block);
    }
    if (lifetime) {
        builder.lifetime_tracker(lifetime);
    }
    return builder.build();
}

myrd::DynamicBlockStoreCreateArgs stress_args(
    std::shared_ptr<const myrd::IBackingStore> backing,
    std::uint64_t block_count = kStressBlockCount) {
    return myrd::DynamicBlockStoreCreateArgs{
        block_count * kBlock,
        static_cast<std::uint32_t>(kSector),
        myrd::DynamicBlockStoreOptions{
            8 * kBlock,
            12 * kBlock,
            4 * kBlock,
            4 * kBlock,
            8,
            64,
            myrd::kMinimumSnapshotCowBytes,
            std::chrono::milliseconds{250},
        },
        myrd::InitialBlockState::BackingUnresident,
        std::move(backing),
        1,
    };
}

std::unique_ptr<myrd::DynamicBlockStore> make_store(
    myrd::DynamicBlockStoreCreateArgs args,
    const std::shared_ptr<TrackingVirtualMemoryApi>& memory_api,
    const std::shared_ptr<myrd::VirtualMemoryCleanupTracker>& cleanup) {
    myrd::DynamicBlockStoreTestDependencies dependencies{};
    dependencies.virtual_memory_api = memory_api;
    dependencies.cleanup_tracker = cleanup;
    auto created = myrd::DynamicBlockStore::create_for_testing(
        std::move(args), std::move(dependencies));
    REQUIRE(created.ok());
    return created.move_value();
}

class PersistenceCoordinator final {
public:
    PersistenceCoordinator(
        myrd::DynamicBlockStore& store,
        ShadowModel& shadow,
        std::shared_ptr<const myrd::test::FakeBackingStore> backing,
        std::shared_ptr<myrd::test::FakeBackingStoreLifetimeTracker> lifetime)
        : store_(store), shadow_(shadow), backing_(std::move(backing)),
          lifetime_(std::move(lifetime)) {}

    bool persist_batch(myrd::DirtyScanCursor& cursor,
                       FailureState& failure,
                       std::uint32_t round,
                       std::uint64_t operation,
                       std::uint64_t& record_count) {
        auto snapshot = store_.copy_dirty_batch(
            bounded_context(), cursor, 4 * kBlock);
        if (!snapshot.ok()) {
            failure.record("snapshot", round, operation,
                           "copy_dirty_batch code=" +
                               std::to_string(snapshot.error().code));
            return false;
        }
        record_count = snapshot.value().size();
        if (snapshot.value().empty()) {
            return true;
        }

        auto builder =
            myrd::test::FakeBackingStoreBuilder::successor_of(*backing_);
        builder.lifetime_tracker(lifetime_);
        for (const auto& record : snapshot.value()) {
            auto* shadow = shadow_.find(record.block_index);
            if (shadow == nullptr) {
                failure.record("snapshot", round, operation,
                               "record for an unselected block");
                return false;
            }
            std::unique_lock lock(shadow->mutex);
            if (!shadow->condition.wait_until(
                    lock,
                    std::chrono::steady_clock::now() + kOperationBound,
                    [&] { return !shadow->mutation_in_progress; })) {
                failure.record("snapshot", round, operation,
                               "shadow mutation did not publish in time");
                return false;
            }
            const auto history = std::find_if(
                shadow->history.begin(), shadow->history.end(),
                [&](const ShadowHistory& item) {
                    return item.version == record.version;
                });
            if (history == shadow->history.end() ||
                history->kind != record.kind ||
                record.base_generation < history->dirty_base_min ||
                record.base_generation > history->dirty_base_max) {
                failure.record("snapshot", round, operation,
                               "version/kind/base absent from independent history");
                return false;
            }
            if (record.kind == myrd::DirtyRecordKind::Data) {
                if (record.data.size() != kBlock ||
                    block_digest(record.data) != history->digest) {
                    failure.record("snapshot", round, operation,
                                   "data payload does not match history");
                    return false;
                }
                myrd::test::FakeBackingStore::BlockData data{};
                std::copy(record.data.begin(), record.data.end(), data.begin());
                builder.data_block(record.block_index, data);
            } else {
                if (!record.data.empty()) {
                    failure.record("snapshot", round, operation,
                                   "zero/discard record carried data");
                    return false;
                }
                builder.all_zero_block(record.block_index);
            }
        }

        const auto successor = builder.build();
        const auto next_generation = generation_ + 1;
        const auto installed = store_.install_backing_view(
            {next_generation, successor});
        if (!installed.ok()) {
            failure.record("installer", round, operation,
                           "install_backing_view code=" +
                               error_code(installed));
            return false;
        }
        backing_ = successor;
        generation_ = next_generation;
        ++install_count_;

        for (const auto& record : snapshot.value()) {
            auto* shadow = shadow_.find(record.block_index);
            if (shadow == nullptr) {
                failure.record("ack", round, operation,
                               "record disappeared from shadow");
                return false;
            }
            std::unique_lock operation_lock(shadow->operation_mutex);
            std::scoped_lock lock(shadow->mutex);
            const bool exact =
                shadow->dirty && shadow->version == record.version &&
                shadow->dirty_kind == record.kind;
            const auto acknowledged = store_.mark_persisted(
                record.block_index, record.version, record.kind,
                generation_);
            if (exact) {
                if (!acknowledged.ok()) {
                    failure.record("ack", round, operation,
                                   "exact ack code=" +
                                       error_code(acknowledged));
                    return false;
                }
                shadow->dirty = false;
                shadow->clean_generation = generation_;
                ++acknowledged_count_;
            } else {
                if (acknowledged.ok() ||
                    acknowledged.error().code !=
                        static_cast<std::uint32_t>(
                            myrd::OperationErrorCode::version_changed)) {
                    failure.record("ack", round, operation,
                                   "stale ack did not return VersionChanged");
                    return false;
                }
                ++stale_ack_count_;
            }
        }
        return true;
    }

    bool drain(FailureState& failure, std::uint32_t checkpoint) {
        for (std::uint32_t pass = 0; pass < 3; ++pass) {
            myrd::DirtyScanCursor cursor{};
            std::uint64_t records_in_pass = 0;
            std::uint64_t operation = 0;
            while (cursor.next_block_index < store_.total_blocks()) {
                const auto before = cursor.next_block_index;
                std::uint64_t records = 0;
                if (!persist_batch(
                        cursor, failure, checkpoint, operation++, records)) {
                    return false;
                }
                records_in_pass += records;
                if (cursor.next_block_index <= before) {
                    failure.record("drain", checkpoint, operation,
                                   "snapshot cursor made no progress");
                    return false;
                }
            }
            if (records_in_pass == 0) {
                return true;
            }
        }
        failure.record("drain", checkpoint, 0,
                       "dirty records remained after bounded passes");
        return false;
    }

    myrd::BackingGeneration generation() const noexcept {
        return generation_;
    }

    std::uint64_t install_count() const noexcept { return install_count_; }
    std::uint64_t acknowledged_count() const noexcept {
        return acknowledged_count_;
    }
    std::uint64_t stale_ack_count() const noexcept {
        return stale_ack_count_;
    }

    void release_backing() noexcept { backing_.reset(); }

private:
    myrd::DynamicBlockStore& store_;
    ShadowModel& shadow_;
    std::shared_ptr<const myrd::test::FakeBackingStore> backing_;
    std::shared_ptr<myrd::test::FakeBackingStoreLifetimeTracker> lifetime_;
    myrd::BackingGeneration generation_{1};
    std::uint64_t install_count_{};
    std::uint64_t acknowledged_count_{};
    std::uint64_t stale_ack_count_{};
};

struct StressCounters {
    std::atomic<std::uint64_t> reads{};
    std::atomic<std::uint64_t> writes{};
    std::atomic<std::uint64_t> zeroes{};
    std::atomic<std::uint64_t> unmaps{};
    std::atomic<std::uint64_t> pressure_rejections{};
    std::atomic<std::uint64_t> pressure_passes{};
    std::atomic<std::uint64_t> pressure_scanned_blocks{};
    std::atomic<std::uint64_t> pressure_evicted_blocks{};
};

void run_data_role(std::uint32_t role,
                   std::uint32_t round,
                   myrd::DynamicBlockStore& store,
                   ShadowModel& shadow,
                   FailureState& failure,
                   StressCounters& counters) {
    DeterministicRandom random(
        kStressSeed ^ (static_cast<std::uint64_t>(round) << 32) ^ role);
    for (std::uint64_t operation = 0;
         operation < kOperationsPerRole && !failure.failed();
         ++operation) {
        auto& block = shadow.by_slot(
            static_cast<std::size_t>(random.next() % kSelectedBlocks.size()));
        const auto offset = block.block_index * kBlock;
        if (role < 2) {
            failure.note("writer", round, operation,
                         "block=" + std::to_string(block.block_index));
            const auto data = make_write_data(
                kStressSeed ^ (static_cast<std::uint64_t>(round) << 48) ^
                (static_cast<std::uint64_t>(role) << 40) ^ operation);
            ShadowMutationGuard mutation(block);
            const auto generation_before = store.backing_generation();
            const auto result = store.write(bounded_context(), offset, data);
            const auto generation_after = store.backing_generation();
            if (result.ok()) {
                mutation.finish_success(
                    myrd::DirtyRecordKind::Data, data,
                    generation_before, generation_after);
                counters.writes.fetch_add(1, std::memory_order_relaxed);
            } else if (result.error().code ==
                       static_cast<std::uint32_t>(
                           myrd::OperationErrorCode::memory_hard_limit)) {
                mutation.finish_without_change();
                counters.pressure_rejections.fetch_add(
                    1, std::memory_order_relaxed);
            } else {
                mutation.finish_without_change();
                failure.record("writer", round, operation,
                               "write code=" + error_code(result));
            }
        } else if (role == 2 || role == 3) {
            const auto* role_name = role == 2 ? "zero" : "unmap";
            failure.note(role_name, round, operation,
                         "block=" + std::to_string(block.block_index));
            const myrd::DataRange range{offset, kBlock};
            ShadowMutationGuard mutation(block);
            const auto generation_before = store.backing_generation();
            const auto result = role == 2
                ? store.zero(bounded_context(),
                      std::span<const myrd::DataRange>(&range, 1))
                : store.unmap(bounded_context(),
                      std::span<const myrd::DataRange>(&range, 1));
            const auto generation_after = store.backing_generation();
            if (!result.ok()) {
                mutation.finish_without_change();
                failure.record(
                    role_name, round, operation,
                    "range mutation code=" + error_code(result));
                continue;
            }
            myrd::test::FakeBackingStore::BlockData zero{};
            zero.fill(std::byte{0});
            mutation.finish_success(
                role == 2 ? myrd::DirtyRecordKind::Zero
                          : myrd::DirtyRecordKind::Discard,
                zero, generation_before, generation_after);
            (role == 2 ? counters.zeroes : counters.unmaps)
                .fetch_add(1, std::memory_order_relaxed);
        } else {
            failure.note("reader", round, operation,
                         "block=" + std::to_string(block.block_index));
            std::array<std::byte, static_cast<std::size_t>(kBlock)> output{};
            std::uint64_t version_before = 0;
            {
                std::scoped_lock lock(block.mutex);
                version_before = block.version;
            }
            const auto result = store.read(bounded_context(), offset, output);
            if (!result.ok()) {
                failure.record("reader", round, operation,
                               "read code=" + error_code(result));
            } else {
                std::unique_lock lock(block.mutex);
                if (!block.condition.wait_until(
                        lock,
                        std::chrono::steady_clock::now() + kOperationBound,
                        [&] { return !block.mutation_in_progress; })) {
                    failure.record("reader", round, operation,
                                   "overlapping mutation did not publish");
                    continue;
                }
                const auto version_after = block.version;
                const auto digest = block_digest(output);
                const bool matched = std::any_of(
                    block.history.begin(), block.history.end(),
                    [&](const ShadowHistory& item) {
                        return item.version >= version_before &&
                               item.version <= version_after &&
                               item.digest == digest;
                    });
                if (!matched) {
                    failure.record(
                        "reader", round, operation,
                        "read was neither an old nor a new complete version");
                } else {
                    counters.reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
        if ((operation & 15u) == 0) {
            std::this_thread::yield();
        }
    }
}

bool settle_eviction_worker(myrd::DynamicBlockStore& store) {
    const auto passes =
        myrd::DynamicBlockStoreTestAccess::eviction_worker_passes(store);
    return myrd::DynamicBlockStoreTestAccess::wait_for_eviction_worker_idle(
        store, passes,
        std::chrono::steady_clock::now() + kCheckpointBound);
}

bool compare_all_selected(myrd::DynamicBlockStore& store,
                          ShadowModel& shadow,
                          FailureState& failure,
                          std::uint32_t checkpoint) {
    for (const auto& owned : shadow.blocks()) {
        auto& block = *owned;
        std::array<std::byte, static_cast<std::size_t>(kBlock)> output{};
        std::scoped_lock lock(block.mutex);
        const auto result = store.read(
            bounded_context(), block.block_index * kBlock, output);
        if (!result.ok()) {
            failure.record("checkpoint-read", checkpoint, block.block_index,
                           "read code=" + error_code(result));
            return false;
        }
        if (!std::equal(
                output.begin(), output.end(), block.bytes.begin())) {
            failure.record("checkpoint-read", checkpoint, block.block_index,
                           "full byte comparison failed");
            return false;
        }
    }
    return true;
}

bool reconcile_quiescent_state(
    myrd::DynamicBlockStore& store,
    ShadowModel& shadow,
    const TrackingVirtualMemoryApi& memory_api,
    myrd::BackingGeneration expected_generation,
    FailureState& failure,
    std::uint32_t checkpoint) {
    if (!settle_eviction_worker(store)) {
        failure.record("checkpoint", checkpoint, 0,
                       "eviction worker did not become idle");
        return false;
    }
    if (!store.validate_invariants_for_test().ok()) {
        failure.record("checkpoint", checkpoint, 0,
                       "store invariant validation failed");
        return false;
    }

    std::uint64_t resident_clean = 0;
    std::uint64_t resident_dirty = 0;
    std::uint64_t zero_clean = 0;
    std::uint64_t zero_dirty = 0;
    for (const auto& owned : shadow.blocks()) {
        auto& shadow_block = *owned;
        std::scoped_lock lock(shadow_block.mutex);
        myrd::BlockEntry entry{};
        if (!myrd::DynamicBlockStoreTestAccess::snapshot_entry(
                store, shadow_block.block_index, entry)) {
            failure.record("checkpoint", checkpoint,
                           shadow_block.block_index,
                           "selected block metadata is absent");
            return false;
        }
        if (!myrd::is_stable_block_state(entry.state) ||
            entry.prior_state != entry.state || entry.transition_owner != 0 ||
            entry.transition_epoch != 0 || entry.pin_count != 0 ||
            entry.version != shadow_block.version ||
            entry.dirty_base_generation > expected_generation ||
            entry.clean_generation > expected_generation ||
            entry.dirty_kind != shadow_block.dirty_kind ||
            (!shadow_block.dirty &&
             entry.clean_generation < shadow_block.clean_generation)) {
            failure.record("checkpoint", checkpoint,
                           shadow_block.block_index,
                           "entry metadata reconciliation failed");
            return false;
        }
        const bool entry_dirty =
            entry.state == myrd::BlockState::ResidentDirty ||
            entry.state == myrd::BlockState::ZeroDirty;
        if (entry_dirty != shadow_block.dirty ||
            (entry_dirty &&
             (entry.dirty_base_generation < shadow_block.dirty_base_min ||
              entry.dirty_base_generation > shadow_block.dirty_base_max))) {
            failure.record("checkpoint", checkpoint,
                           shadow_block.block_index,
                           "dirty state/base differs from shadow");
            return false;
        }
        switch (entry.state) {
        case myrd::BlockState::ResidentClean:
            ++resident_clean;
            break;
        case myrd::BlockState::ResidentDirty:
            ++resident_dirty;
            break;
        case myrd::BlockState::ZeroClean:
            ++zero_clean;
            break;
        case myrd::BlockState::ZeroDirty:
            ++zero_dirty;
            break;
        case myrd::BlockState::Unresident:
            break;
        case myrd::BlockState::Loading:
        case myrd::BlockState::Committing:
        case myrd::BlockState::Evicting:
            failure.record("checkpoint", checkpoint,
                           shadow_block.block_index,
                           "transient state survived quiescence");
            return false;
        }
    }

    const auto stats = store.stats();
    if (stats.backing_generation != expected_generation ||
        store.allocated_leaf_count_for_test() != 3 ||
        stats.resident_clean_blocks != resident_clean ||
        stats.resident_dirty_blocks != resident_dirty ||
        stats.zero_clean_blocks != zero_clean ||
        stats.zero_dirty_blocks != zero_dirty || stats.loading_blocks != 0 ||
        stats.committed_bytes !=
            (resident_clean + resident_dirty) * kBlock ||
        stats.committed_bytes != memory_api.active_committed_bytes() ||
        stats.reserved_bytes != store.reservation_size_bytes() ||
        memory_api.active_reservations() != 1 ||
        myrd::DynamicBlockStoreTestAccess::commit_reservation_bytes(store) !=
            0) {
        failure.record("checkpoint", checkpoint, 0,
                       "store/arena/accounting gauges disagree");
        return false;
    }
    return true;
}

void run_stress_round(std::uint32_t round,
                      myrd::DynamicBlockStore& store,
                      ShadowModel& shadow,
                      PersistenceCoordinator& persistence,
                      FailureState& failure,
                      StressCounters& counters) {
    constexpr std::uint32_t kDataRoles = 6;
    std::atomic<std::uint32_t> active_roles{kDataRoles};
    std::barrier start(static_cast<std::ptrdiff_t>(kDataRoles + 2));
    std::vector<std::thread> threads;
    threads.reserve(kDataRoles + 2);
    for (std::uint32_t role = 0; role < kDataRoles; ++role) {
        threads.emplace_back([&, role] {
            start.arrive_and_wait();
            try {
                run_data_role(
                    role, round, store, shadow, failure, counters);
            } catch (...) {
                failure.record("data-exception", round, role,
                               "unexpected exception");
            }
            active_roles.fetch_sub(1, std::memory_order_acq_rel);
        });
    }
    threads.emplace_back([&] {
        start.arrive_and_wait();
        try {
            myrd::DirtyScanCursor cursor{};
            std::uint64_t operation = 0;
            while (active_roles.load(std::memory_order_acquire) != 0 &&
                   !failure.failed()) {
                failure.note(
                    "persistence", round, operation,
                    "cursor=" + std::to_string(cursor.next_block_index));
                std::uint64_t records = 0;
                if (!persistence.persist_batch(
                        cursor, failure, round, operation++, records)) {
                    break;
                }
                if (cursor.next_block_index == store.total_blocks()) {
                    cursor.next_block_index = 0;
                }
                if (records == 0) {
                    std::this_thread::yield();
                }
            }
        } catch (...) {
            failure.record("persistence-exception", round, 0,
                           "unexpected exception");
        }
    });
    threads.emplace_back([&] {
        start.arrive_and_wait();
        try {
            std::uint64_t operation = 0;
            bool pressure_pass_executed = false;
            while ((!pressure_pass_executed ||
                    active_roles.load(std::memory_order_acquire) != 0) &&
                   !failure.failed() && operation < 256) {
                failure.note("pressure", round, operation, "target_blocks=4");
                const auto pressure =
                    myrd::DynamicBlockStoreTestAccess::reclaim_clean_to(
                        store, 4 * kBlock);
                if (pressure.invariant_failures != 0) {
                    failure.record("pressure", round, operation,
                                   "eviction invariant failure");
                    break;
                }
                counters.pressure_passes.fetch_add(
                    1, std::memory_order_relaxed);
                counters.pressure_scanned_blocks.fetch_add(
                    pressure.scanned_blocks, std::memory_order_relaxed);
                counters.pressure_evicted_blocks.fetch_add(
                    pressure.evicted_blocks, std::memory_order_relaxed);
                pressure_pass_executed = true;
                ++operation;
                std::this_thread::yield();
            }
        } catch (...) {
            failure.record("pressure-exception", round, 0,
                           "unexpected exception");
        }
    });
    for (auto& thread : threads) {
        thread.join();
    }
}

struct ProcessSample {
    std::uint64_t handles{};
    std::uint64_t threads{};
    std::uint64_t private_bytes{};
    bool ok{};
};

std::uint64_t current_process_thread_count() {
    const auto snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    std::uint64_t count = 0;
    if (::Thread32First(snapshot, &entry) != FALSE) {
        do {
            if (entry.th32OwnerProcessID == ::GetCurrentProcessId()) {
                ++count;
            }
            entry.dwSize = sizeof(entry);
        } while (::Thread32Next(snapshot, &entry) != FALSE);
    }
    ::CloseHandle(snapshot);
    return count;
}

ProcessSample sample_process() {
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    DWORD handles = 0;
    const auto threads = current_process_thread_count();
    const bool memory_ok = ::GetProcessMemoryInfo(
        ::GetCurrentProcess(),
        reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory),
        sizeof(memory)) != FALSE;
    const bool handles_ok =
        ::GetProcessHandleCount(::GetCurrentProcess(), &handles) != FALSE;
    return ProcessSample{
        handles,
        threads,
        static_cast<std::uint64_t>(memory.PrivateUsage),
        memory_ok && handles_ok && threads != 0,
    };
}

void run_lifecycle_cycle(
    const std::shared_ptr<TrackingVirtualMemoryApi>& memory_api,
    std::uint64_t cycle) {
    auto lifetime =
        std::make_shared<myrd::test::FakeBackingStoreLifetimeTracker>();
    auto cleanup = std::make_shared<myrd::VirtualMemoryCleanupTracker>();
    auto backing = make_zero_backing(32, 0xA500 + cycle, lifetime);
    auto args = stress_args(backing, 32);
    args.options.eviction_low_watermark_bytes = kBlock;
    args.options.soft_limit_bytes = 2 * kBlock;
    args.options.hard_limit_bytes = 4 * kBlock;
    auto store = make_store(std::move(args), memory_api, cleanup);

    const auto data = make_write_data(0xB600 + cycle);
    REQUIRE(store->write(bounded_context(), 0, data).ok());
    std::array<std::byte, static_cast<std::size_t>(kBlock)> output{};
    REQUIRE(store->read(bounded_context(), 0, output).ok());
    REQUIRE(std::equal(output.begin(), output.end(), data.begin()));
    const myrd::DataRange range{kBlock, kBlock};
    REQUIRE(store->zero(
        bounded_context(), std::span<const myrd::DataRange>(&range, 1)).ok());
    REQUIRE(store->validate_invariants_for_test().ok());
    store->close();
    REQUIRE(!store->worker_running_for_test());
    store.reset();
    backing.reset();

    REQUIRE_EQ(memory_api->active_reservations(), 0ull);
    REQUIRE_EQ(memory_api->active_committed_bytes(), 0ull);
    REQUIRE_EQ(cleanup->release_failures(), 0ull);
    REQUIRE_EQ(lifetime->live_count(), 0ull);
    REQUIRE_EQ(lifetime->destroyed_count(), 1ull);
}

} // namespace

TEST_CASE(m2_stress_fixed_seed_multirole_preserves_model_and_invariants) {
    auto memory_api = std::make_shared<TrackingVirtualMemoryApi>();
    auto cleanup = std::make_shared<myrd::VirtualMemoryCleanupTracker>();
    auto lifetime =
        std::make_shared<myrd::test::FakeBackingStoreLifetimeTracker>();
    auto backing = make_zero_backing(
        kStressBlockCount, kStressSeed ^ 0x1010u, lifetime);
    auto store = make_store(stress_args(backing), memory_api, cleanup);
    ShadowModel shadow;
    PersistenceCoordinator persistence(
        *store, shadow, backing, lifetime);
    backing.reset();
    FailureState failure;
    StressCounters counters;

    // Seed more clean resident blocks than the pressure target before the
    // concurrent rounds. Without this deterministic starting condition, a
    // fast Release run can let all data roles finish while committed bytes are
    // still at or below the target, causing reclaim_clean_to() to return
    // without scanning anything. The stress assertions are about the pressure
    // path, so the path must be scheduled rather than left to a race.
    for (std::size_t index = 0; index < 8; ++index) {
        std::array<std::byte, static_cast<std::size_t>(kBlock)> output{};
        REQUIRE(store->read(
            bounded_context(), kSelectedBlocks[index] * kBlock, output).ok());
    }

    for (std::uint32_t round = 0; round < kStressRounds; ++round) {
        run_stress_round(
            round, *store, shadow, persistence, failure, counters);
        if (!failure.failed()) {
            reconcile_quiescent_state(
                *store, shadow, *memory_api, persistence.generation(),
                failure, round * 2);
        }
        if (!failure.failed()) {
            persistence.drain(failure, round);
        }
        if (!failure.failed()) {
            const auto pressure =
                myrd::DynamicBlockStoreTestAccess::reclaim_clean_to(*store, 0);
            if (pressure.invariant_failures != 0) {
                failure.record("checkpoint-pressure", round, 0,
                               "eviction invariant failure");
            }
        }
        if (!failure.failed()) {
            compare_all_selected(*store, shadow, failure, round);
        }
        if (!failure.failed()) {
            reconcile_quiescent_state(
                *store, shadow, *memory_api, persistence.generation(),
                failure, round * 2 + 1);
        }
        if (failure.failed()) {
            break;
        }
    }

    if (failure.failed()) {
        std::cerr << failure.message()
                  << "\nM2_STRESS_COUNTERS reads=" << counters.reads.load()
                  << " writes=" << counters.writes.load()
                  << " zeroes=" << counters.zeroes.load()
                  << " unmaps=" << counters.unmaps.load()
                  << " pressure_rejections="
                  << counters.pressure_rejections.load()
                  << " pressure_passes=" << counters.pressure_passes.load()
                  << " pressure_scanned="
                  << counters.pressure_scanned_blocks.load()
                  << " pressure_evicted="
                  << counters.pressure_evicted_blocks.load()
                  << " installs=" << persistence.install_count()
                  << " exact_acks=" << persistence.acknowledged_count()
                  << " stale_acks=" << persistence.stale_ack_count()
                  << std::endl;
    }
    std::cout << "M2_STRESS_SUMMARY seed=0x" << std::hex << kStressSeed
              << std::dec
              << " reads=" << counters.reads.load()
              << " writes=" << counters.writes.load()
              << " zeroes=" << counters.zeroes.load()
              << " unmaps=" << counters.unmaps.load()
              << " pressure_rejections="
              << counters.pressure_rejections.load()
              << " pressure_passes=" << counters.pressure_passes.load()
              << " pressure_scanned="
              << counters.pressure_scanned_blocks.load()
              << " pressure_evicted="
              << counters.pressure_evicted_blocks.load()
              << " installs=" << persistence.install_count()
              << " exact_acks=" << persistence.acknowledged_count()
              << " stale_acks=" << persistence.stale_ack_count()
              << " generation=" << persistence.generation() << '\n';
    REQUIRE(!failure.failed());
    REQUIRE(counters.reads.load(std::memory_order_relaxed) != 0);
    REQUIRE(counters.writes.load(std::memory_order_relaxed) != 0);
    REQUIRE(counters.zeroes.load(std::memory_order_relaxed) != 0);
    REQUIRE(counters.unmaps.load(std::memory_order_relaxed) != 0);
    REQUIRE(counters.pressure_passes.load(std::memory_order_relaxed) != 0);
    REQUIRE(counters.pressure_rejections.load(
                std::memory_order_relaxed) != 0);
    REQUIRE(counters.pressure_scanned_blocks.load(
                std::memory_order_relaxed) != 0);
    REQUIRE(counters.pressure_evicted_blocks.load(
                std::memory_order_relaxed) != 0);
    REQUIRE(persistence.install_count() != 0);
    REQUIRE(persistence.acknowledged_count() != 0);
    REQUIRE(memory_api->decommit_calls() != 0);
    REQUIRE(store->stats().evicted_blocks_total >=
            counters.pressure_evicted_blocks.load(
                std::memory_order_relaxed));
    REQUIRE(store->stats().hard_limit_rejections >=
            counters.pressure_rejections.load(
                std::memory_order_relaxed));

    const auto backing_count = persistence.install_count() + 1;
    store->close();
    REQUIRE(!store->worker_running_for_test());
    store.reset();
    persistence.release_backing();
    REQUIRE_EQ(cleanup->release_failures(), 0ull);
    REQUIRE_EQ(memory_api->active_reservations(), 0ull);
    REQUIRE_EQ(memory_api->active_committed_bytes(), 0ull);
    REQUIRE_EQ(lifetime->live_count(), 0ull);
    REQUIRE_EQ(lifetime->destroyed_count(), backing_count);
}

TEST_CASE(m2_stress_repeated_lifecycle_returns_resources_to_baseline) {
    constexpr std::uint64_t kCycles = 20;
    constexpr std::uint64_t kAllocatorNoiseAllowance = 1ull * 1024ull * 1024ull;
    auto memory_api = std::make_shared<TrackingVirtualMemoryApi>();

    run_lifecycle_cycle(memory_api, 0);
    std::array<ProcessSample, 5> before_samples{};
    for (auto& sample : before_samples) {
        sample = sample_process();
        REQUIRE(sample.ok);
    }
    const auto before = before_samples.back();
    const auto before_private_min = std::min_element(
        before_samples.begin(), before_samples.end(),
        [](const ProcessSample& left, const ProcessSample& right) {
            return left.private_bytes < right.private_bytes;
        })->private_bytes;
    const auto before_private_max = std::max_element(
        before_samples.begin(), before_samples.end(),
        [](const ProcessSample& left, const ProcessSample& right) {
            return left.private_bytes < right.private_bytes;
        })->private_bytes;
    for (const auto& sample : before_samples) {
        REQUIRE_EQ(sample.handles, before.handles);
        REQUIRE_EQ(sample.threads, before.threads);
    }
    const auto releases_before = memory_api->release_calls();
    for (std::uint64_t cycle = 1; cycle <= kCycles; ++cycle) {
        run_lifecycle_cycle(memory_api, cycle);
    }
    std::array<ProcessSample, 5> after_samples{};
    for (auto& sample : after_samples) {
        sample = sample_process();
        REQUIRE(sample.ok);
    }
    const auto after = after_samples.back();
    const auto after_private_max = std::max_element(
        after_samples.begin(), after_samples.end(),
        [](const ProcessSample& left, const ProcessSample& right) {
            return left.private_bytes < right.private_bytes;
        })->private_bytes;
    for (const auto& sample : after_samples) {
        REQUIRE_EQ(sample.handles, before.handles);
        REQUIRE_EQ(sample.threads, before.threads);
    }

    const auto private_growth = after_private_max > before_private_max
        ? after_private_max - before_private_max
        : 0;
    const auto baseline_noise = before_private_max - before_private_min;
    const auto private_tolerance =
        baseline_noise + kAllocatorNoiseAllowance;
    std::cout << "M2_RESOURCE_SUMMARY cycles=" << kCycles
              << " handles_before=" << before.handles
              << " handles_after=" << after.handles
              << " threads_before=" << before.threads
              << " threads_after=" << after.threads
              << " private_before=" << before.private_bytes
              << " private_after=" << after.private_bytes
              << " private_growth=" << private_growth
              << " baseline_noise=" << baseline_noise
              << " private_tolerance=" << private_tolerance
              << " reserves=" << memory_api->reserve_calls()
              << " releases=" << memory_api->release_calls() << '\n';

    REQUIRE_EQ(after.handles, before.handles);
    REQUIRE_EQ(after.threads, before.threads);
    REQUIRE(private_growth <= private_tolerance);
    REQUIRE_EQ(memory_api->release_calls() - releases_before, kCycles);
    REQUIRE_EQ(memory_api->reserve_calls(), memory_api->release_calls());
    REQUIRE_EQ(memory_api->active_reservations(), 0ull);
    REQUIRE_EQ(memory_api->active_committed_bytes(), 0ull);
}
