#pragma once

#include "src/storage/backing_store.h"
#include "src/storage/block_constants.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace myrd::test {

class FakeBackingStoreGate final {
public:
    Result<void> wait(const OperationContext& context) {
        std::unique_lock lock(mutex_);
        ++entered_count_;
        condition_.notify_all();

        const bool opened = condition_.wait_until(lock, context.stop_token,
            context.deadline, [this] { return open_; });
        if (!opened) {
            const auto live = check_operation_context(context);
            if (!live) return live;
        }
        return Result<void>::success();
    }

    bool wait_until_entered(std::uint64_t count,
        std::chrono::steady_clock::time_point deadline) const {
        std::unique_lock lock(mutex_);
        return condition_.wait_until(
            lock, deadline, [this, count] { return entered_count_ >= count; });
    }

    void open() {
        {
            std::scoped_lock lock(mutex_);
            open_ = true;
        }
        condition_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable_any condition_;
    std::uint64_t entered_count_ = 0;
    bool open_ = false;
};

class FakeBackingStoreLifetimeTracker final {
public:
    std::uint64_t live_count() const noexcept {
        return live_count_.load(std::memory_order_relaxed);
    }

    std::uint64_t destroyed_count() const noexcept {
        return destroyed_count_.load(std::memory_order_relaxed);
    }

private:
    friend class FakeBackingStore;

    void on_construct() noexcept {
        live_count_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_destroy() noexcept {
        live_count_.fetch_sub(1, std::memory_order_relaxed);
        destroyed_count_.fetch_add(1, std::memory_order_relaxed);
    }

    std::atomic<std::uint64_t> live_count_{0};
    std::atomic<std::uint64_t> destroyed_count_{0};
};

class FakeBackingStoreBuilder;

class FakeBackingStore final : public IBackingStore {
public:
    using BlockData =
        std::array<std::byte, static_cast<std::size_t>(kBlockSizeBytes)>;

    ~FakeBackingStore() override {
        if (lifetime_tracker_) lifetime_tracker_->on_destroy();
    }

    FakeBackingStore(const FakeBackingStore&) = delete;
    FakeBackingStore& operator=(const FakeBackingStore&) = delete;

    Result<BlockLoadResult> load_block(const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const override {
        if (destination.size() != kBlockSizeBytes) {
            return Result<BlockLoadResult>::failure({
                ErrorDomain::storage,
                static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
                L"FakeBackingStore::load_block",
                L"destination must be exactly one block"});
        }

        const auto live = check_operation_context(context);
        if (!live) {
            return Result<BlockLoadResult>::failure(live.error());
        }

        const auto occurrence = record_load(block_index);

        const auto gate = gates_.find(block_index);
        if (gate != gates_.end()) {
            const auto released = gate->second->wait(context);
            if (!released) {
                return Result<BlockLoadResult>::failure(released.error());
            }
        }

        const auto failure = failures_.find(block_index);
        if (fail_blocks_.contains(block_index) ||
            (global_failure_occurrence_ &&
                *global_failure_occurrence_ == occurrence.total) ||
            (failure != failures_.end() &&
                failure->second == occurrence.block)) {
            return Result<BlockLoadResult>::failure({
                ErrorDomain::storage,
                static_cast<std::uint32_t>(
                    OperationErrorCode::backing_read_failed),
                L"FakeBackingStore::load_block",
                L"injected backing-store failure"});
        }

        if (all_zero_blocks_.contains(block_index)) {
            return Result<BlockLoadResult>::success({BlockLoadKind::AllZero});
        }

        const auto data = data_blocks_.find(block_index);
        if (data != data_blocks_.end()) {
            std::copy(data->second.begin(), data->second.end(),
                destination.begin());
        } else {
            for (std::size_t offset = 0; offset < destination.size(); ++offset) {
                destination[offset] =
                    deterministic_byte(seed_, block_index, offset);
            }
        }
        return Result<BlockLoadResult>::success({BlockLoadKind::Data});
    }

    std::uint64_t total_load_count() const noexcept {
        std::scoped_lock lock(state_mutex_);
        return total_load_count_;
    }

    std::uint64_t block_load_count(
        std::uint64_t block_index) const noexcept {
        std::scoped_lock lock(state_mutex_);
        const auto found = block_load_counts_.find(block_index);
        return found == block_load_counts_.end() ? 0 : found->second;
    }

    static std::byte deterministic_byte(std::uint64_t seed,
        std::uint64_t block_index, std::size_t offset) noexcept {
        auto value = seed ^ (block_index + 0x9E37'79B9'7F4A'7C15ull);
        value ^= static_cast<std::uint64_t>(offset) +
                 0xBF58'476D'1CE4'E5B9ull + (value << 6) + (value >> 2);
        value ^= value >> 30;
        value *= 0xBF58'476D'1CE4'E5B9ull;
        value ^= value >> 27;
        value *= 0x94D0'49BB'1331'11EBull;
        value ^= value >> 31;
        return static_cast<std::byte>(value & 0xFFu);
    }

private:
    friend class FakeBackingStoreBuilder;

    FakeBackingStore(std::uint64_t seed,
        std::unordered_map<std::uint64_t, BlockData> data_blocks,
        std::unordered_set<std::uint64_t> all_zero_blocks,
        std::unordered_set<std::uint64_t> fail_blocks,
        std::unordered_map<std::uint64_t, std::uint64_t> failures,
        std::optional<std::uint64_t> global_failure_occurrence,
        std::unordered_map<std::uint64_t,
            std::shared_ptr<FakeBackingStoreGate>> gates,
        std::shared_ptr<FakeBackingStoreLifetimeTracker> lifetime_tracker)
        : seed_(seed), data_blocks_(std::move(data_blocks)),
          all_zero_blocks_(std::move(all_zero_blocks)),
          fail_blocks_(std::move(fail_blocks)), failures_(std::move(failures)),
          global_failure_occurrence_(global_failure_occurrence),
          gates_(std::move(gates)),
          lifetime_tracker_(std::move(lifetime_tracker)) {
        if (lifetime_tracker_) lifetime_tracker_->on_construct();
    }

    struct LoadOccurrence {
        std::uint64_t total;
        std::uint64_t block;
    };

    LoadOccurrence record_load(std::uint64_t block_index) const {
        std::scoped_lock lock(state_mutex_);
        return {++total_load_count_, ++block_load_counts_[block_index]};
    }

    const std::uint64_t seed_;
    const std::unordered_map<std::uint64_t, BlockData> data_blocks_;
    const std::unordered_set<std::uint64_t> all_zero_blocks_;
    const std::unordered_set<std::uint64_t> fail_blocks_;
    const std::unordered_map<std::uint64_t, std::uint64_t> failures_;
    const std::optional<std::uint64_t> global_failure_occurrence_;
    const std::unordered_map<std::uint64_t,
        std::shared_ptr<FakeBackingStoreGate>> gates_;
    const std::shared_ptr<FakeBackingStoreLifetimeTracker> lifetime_tracker_;

    mutable std::mutex state_mutex_;
    mutable std::uint64_t total_load_count_ = 0;
    mutable std::unordered_map<std::uint64_t, std::uint64_t>
        block_load_counts_;
};

class FakeBackingStoreBuilder final {
public:
    explicit FakeBackingStoreBuilder(std::uint64_t seed) : seed_(seed) {}

    static FakeBackingStoreBuilder successor_of(
        const FakeBackingStore& predecessor) {
        FakeBackingStoreBuilder builder(predecessor.seed_);
        builder.data_blocks_ = predecessor.data_blocks_;
        builder.all_zero_blocks_ = predecessor.all_zero_blocks_;
        return builder;
    }

    FakeBackingStoreBuilder& data_block(
        std::uint64_t block_index, const FakeBackingStore::BlockData& data) {
        data_blocks_[block_index] = data;
        all_zero_blocks_.erase(block_index);
        return *this;
    }

    FakeBackingStoreBuilder& all_zero_block(std::uint64_t block_index) {
        all_zero_blocks_.insert(block_index);
        data_blocks_.erase(block_index);
        return *this;
    }

    FakeBackingStoreBuilder& fail_block_on_load(
        std::uint64_t block_index, std::uint64_t occurrence) {
        failures_[block_index] = occurrence;
        return *this;
    }

    FakeBackingStoreBuilder& fail_block(std::uint64_t block_index) {
        fail_blocks_.insert(block_index);
        return *this;
    }

    FakeBackingStoreBuilder& fail_on_load(std::uint64_t occurrence) {
        global_failure_occurrence_ = occurrence;
        return *this;
    }

    FakeBackingStoreBuilder& gate_block(std::uint64_t block_index,
        std::shared_ptr<FakeBackingStoreGate> gate) {
        gates_[block_index] = std::move(gate);
        return *this;
    }

    FakeBackingStoreBuilder& lifetime_tracker(
        std::shared_ptr<FakeBackingStoreLifetimeTracker> tracker) {
        lifetime_tracker_ = std::move(tracker);
        return *this;
    }

    std::shared_ptr<const FakeBackingStore> build() const {
        return std::shared_ptr<const FakeBackingStore>(new FakeBackingStore(
            seed_, data_blocks_, all_zero_blocks_, fail_blocks_, failures_,
            global_failure_occurrence_, gates_, lifetime_tracker_));
    }

private:
    std::uint64_t seed_;
    std::unordered_map<std::uint64_t, FakeBackingStore::BlockData>
        data_blocks_;
    std::unordered_set<std::uint64_t> all_zero_blocks_;
    std::unordered_set<std::uint64_t> fail_blocks_;
    std::unordered_map<std::uint64_t, std::uint64_t> failures_;
    std::optional<std::uint64_t> global_failure_occurrence_;
    std::unordered_map<std::uint64_t,
        std::shared_ptr<FakeBackingStoreGate>> gates_;
    std::shared_ptr<FakeBackingStoreLifetimeTracker> lifetime_tracker_;
};

} // namespace myrd::test
