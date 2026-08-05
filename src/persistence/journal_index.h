#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/file_identity.h"
#include "src/persistence/file_io.h"
#include "src/persistence/journal_format.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace myrd::persistence {

enum class JournalIndexLookupKind : std::uint8_t {
    Absent = 0,
    Data = 1,
    Zero = 2,
    Discard = 3,
};

struct JournalIndexLookupResult final {
    JournalIndexLookupKind kind{JournalIndexLookupKind::Absent};
    JournalIndexLeafEntry descriptor{};

    bool operator==(const JournalIndexLookupResult&) const noexcept = default;
};

struct JournalIndexView final {
    std::shared_ptr<IPersistenceFile> file{};
    WindowsFileIdentity file_identity{};
    JournalFileIdentity journal_identity{};
    JournalEpoch epoch{0};
    CheckpointGeneration root_generation{0};
    std::uint64_t committed_length{0};
    PersistentIndexRootDescriptor root{};
};

struct JournalIndexCacheOptions final {
    std::uint64_t max_bytes{0};
    std::uint64_t max_entries{0};
};

struct JournalIndexCacheStats final {
    std::uint64_t hits{0};
    std::uint64_t misses{0};
    std::uint64_t evictions{0};
    std::uint64_t read_calls{0};
    std::uint64_t current_entries{0};
    std::uint64_t current_bytes{0};
    std::uint64_t peak_entries{0};
    std::uint64_t peak_bytes{0};
};

class JournalIndexReader final {
public:
    static Result<std::unique_ptr<JournalIndexReader>> create(
        JournalIndexCacheOptions options);

    ~JournalIndexReader();
    JournalIndexReader(JournalIndexReader&&) noexcept;
    JournalIndexReader& operator=(JournalIndexReader&&) noexcept;
    JournalIndexReader(const JournalIndexReader&) = delete;
    JournalIndexReader& operator=(const JournalIndexReader&) = delete;

    Result<JournalIndexLookupResult> lookup(
        const OperationContext& context,
        const JournalIndexView& view,
        std::uint64_t block_index);
    Result<JournalIndexLookupResult> lookup(
        const JournalIndexView& view,
        std::uint64_t block_index);

    Result<DecodedJournalIndexNode> read_node(
        const OperationContext& context,
        const JournalIndexView& view,
        std::uint64_t node_offset,
        std::optional<std::uint32_t> expected_crc32c = std::nullopt);

    [[nodiscard]] JournalIndexCacheStats cache_stats() const;
    void clear_cache();

#if defined(MYRD_ENABLE_TEST_SEAMS)
    Result<void> hold_cache_lock_for_test(
        std::atomic<bool>& entered,
        std::chrono::milliseconds duration);
#endif

private:
    struct Impl;
    explicit JournalIndexReader(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

struct JournalIndexBuilderOptions final {
    JournalIndexCacheOptions reader_cache{32 * kIndexNodeBytes, 32};
    std::uint64_t max_new_nodes{0};
    // Hard budget for the fixed 4096-byte node images represented by a plan.
    // Transient caller/input memory is not part of this serialized-image budget.
    std::uint64_t max_plan_image_bytes{0};
};

struct JournalIndexPlanChild final {
    bool planned{false};
    std::uint32_t plan_index{0};
    std::uint16_t level{0};
    std::uint64_t minimum_key{0};
    std::uint64_t maximum_key{0};
    std::uint64_t offset{0};
    std::uint64_t length{0};
    std::uint32_t crc32c{0};
    CheckpointGeneration generation{0};

    bool operator==(const JournalIndexPlanChild&) const noexcept = default;
};

struct JournalIndexPlanNode final {
    std::uint16_t level{0};
    std::vector<JournalIndexLeafEntry> leaf_entries{};
    std::vector<JournalIndexPlanChild> children{};

    bool operator==(const JournalIndexPlanNode&) const noexcept = default;
};

struct JournalIndexPlanBaseIdentity final {
    WindowsFileIdentity file_identity{};
    JournalFileIdentity journal_identity{};
    JournalEpoch epoch{0};
    CheckpointGeneration root_generation{0};
    std::uint64_t committed_length{0};
    PersistentIndexRootDescriptor root{};

    bool operator==(const JournalIndexPlanBaseIdentity&) const noexcept = default;
};

class JournalIndexPlanBuilderDetail;

class JournalIndexBuildPlan final {
public:
    JournalIndexBuildPlan(const JournalIndexBuildPlan&) = default;
    JournalIndexBuildPlan& operator=(const JournalIndexBuildPlan&) = default;
    JournalIndexBuildPlan(JournalIndexBuildPlan&&) noexcept = default;
    JournalIndexBuildPlan& operator=(JournalIndexBuildPlan&&) noexcept = default;

    [[nodiscard]] CheckpointGeneration checkpoint_generation() const noexcept {
        return generation;
    }
    [[nodiscard]] std::uint64_t checkpoint_operation_id() const noexcept {
        return operation_id;
    }
    [[nodiscard]] const JournalIndexPlanBaseIdentity& base_identity() const noexcept {
        return base;
    }
    [[nodiscard]] const JournalIndexPlanChild& root_reference() const noexcept {
        return root;
    }
    [[nodiscard]] const std::vector<JournalIndexPlanNode>& planned_nodes() const noexcept {
        return nodes;
    }

    bool operator==(const JournalIndexBuildPlan&) const noexcept = default;

private:
    JournalIndexBuildPlan() = default;

    CheckpointGeneration generation{0};
    std::uint64_t operation_id{0};
    JournalIndexPlanBaseIdentity base{};
    JournalIndexPlanChild root{};
    std::vector<JournalIndexPlanNode> nodes{};

    friend class JournalIndexPlanBuilderDetail;
    friend class JournalIndexBuilder;
};

struct JournalIndexAssignedNode final {
    std::uint64_t offset{0};
    std::uint32_t crc32c{0};
    std::uint16_t level{0};
    std::array<std::byte, kIndexNodeBytes> bytes{};
};

struct JournalIndexAssignedPlan final {
    PersistentIndexRootDescriptor root{};
    std::uint64_t next_offset{0};
    std::vector<JournalIndexAssignedNode> nodes{};
};

class JournalIndexBuilder final {
public:
    static Result<std::unique_ptr<JournalIndexBuilder>> create(
        JournalIndexBuilderOptions options);

    ~JournalIndexBuilder();
    JournalIndexBuilder(JournalIndexBuilder&&) noexcept;
    JournalIndexBuilder& operator=(JournalIndexBuilder&&) noexcept;
    JournalIndexBuilder(const JournalIndexBuilder&) = delete;
    JournalIndexBuilder& operator=(const JournalIndexBuilder&) = delete;

    Result<JournalIndexBuildPlan> plan(
        const OperationContext& context,
        const JournalIndexView& base_view,
        std::span<const JournalIndexLeafEntry> updates,
        CheckpointGeneration new_generation,
        std::uint64_t operation_id);

    Result<JournalIndexAssignedPlan> assign_offsets(
        const JournalIndexBuildPlan& plan,
        const JournalIndexView& expected_base_view,
        std::uint64_t first_offset,
        std::uint64_t first_sequence,
        std::uint64_t committed_bound);

private:
    struct Impl;
    explicit JournalIndexBuilder(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

#if defined(MYRD_ENABLE_TEST_SEAMS)
namespace test {
struct JournalIndexGuardTestCounters final {
    std::uint64_t visited_calls{0};
    std::uint64_t depth_calls{0};
};
void reset_journal_index_guard_test_counters();
[[nodiscard]] JournalIndexGuardTestCounters
journal_index_guard_test_counters();
[[nodiscard]] Result<void> journal_index_test_rejects_visited_offset(
    std::span<const std::uint64_t> visited,
    std::uint64_t candidate);
[[nodiscard]] Result<void> journal_index_test_rejects_depth_overflow(
    std::uint32_t depth,
    std::uint16_t node_level);
}  // namespace test
#endif

}  // namespace myrd::persistence
