#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/file_identity.h"
#include "src/persistence/file_io.h"
#include "src/persistence/journal_format.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>

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
