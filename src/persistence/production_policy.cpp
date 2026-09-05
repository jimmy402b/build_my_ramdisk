#include "src/persistence/production_policy.h"

#include "src/persistence/journal_format.h"
#include "src/persistence/journal_index.h"
#include "src/persistence/vhdx_reader.h"

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

} // namespace myrd::persistence
