#include "src/persistence/production_policy.h"

#include "src/persistence/journal_format.h"
#include "src/persistence/journal_index.h"
#include "src/persistence/vhdx_reader.h"

#include <algorithm>

namespace myrd::persistence {

PersistenceLimits production_persistence_limits(
    const std::uint64_t virtual_bytes) noexcept {
    constexpr std::uint64_t mib = 1024ULL * 1024ULL;
    constexpr std::uint64_t journal_bytes = 2ULL * 1024ULL * mib;
    constexpr std::uint64_t index_bytes = 64ULL * mib;
    constexpr std::uint64_t bat_bytes = 16ULL * mib;
    return {
        virtual_bytes,
        journal_bytes,
        journal_bytes / kJournalAlignment - 1,
        index_bytes,
        index_bytes / kIndexNodeBytes,
        bat_bytes,
        bat_bytes / kVhdxBatPageBytes,
        8ULL * mib,
        8ULL * mib};
}

PersistenceLimits recovery_persistence_limits(
    const std::uint64_t virtual_bytes,
    const std::uint64_t ram_limit_bytes) noexcept {
    auto limits = production_persistence_limits(virtual_bytes);
    if (ram_limit_bytes == 0 || virtual_bytes == 0) return limits;
    // The dirty set is bounded by both the store's hard limit and the disk it
    // populates.  Twice that plus overhead is the worker's own rule
    // (`default_recovery_options`) and the measured worst case, so the reader
    // and the writer answer "how big may a legal journal be?" identically.
    constexpr std::uint64_t kJournalOverheadBytes = 64ULL * 1024ULL * 1024ULL;
    const std::uint64_t dirty_bound =
        (std::min)(ram_limit_bytes, virtual_bytes);
    const std::uint64_t journal_bytes = (std::max)(
        limits.max_journal_committed_bytes,
        dirty_bound * 2 + kJournalOverheadBytes);
    limits.max_journal_committed_bytes = journal_bytes;
    // One record per 16 KiB of journal, the same ratio the worker uses, kept
    // as a floor so a small budget's record bound is unchanged.  It must stay
    // under `max_journal_committed_bytes / kJournalAlignment` or
    // `validate_limits` refuses the whole policy.
    limits.max_journal_records = (std::max)(
        limits.max_journal_records, journal_bytes / (16ULL * 1024ULL));
    return limits;
}

} // namespace myrd::persistence
