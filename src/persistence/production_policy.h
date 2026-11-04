#pragma once

#include "src/persistence/persistence_types.h"

#include <cstdint>

namespace myrd::persistence {

// One finite recovery/inspection policy shared by product workers, recovery
// adapters and acceptance executables.  Callers must not duplicate these
// bounds at a composition root.
PersistenceLimits production_persistence_limits(
    std::uint64_t virtual_bytes) noexcept;

// The same policy for a profile that carries a memory budget.
//
// `production_persistence_limits` fixes `max_journal_committed_bytes` at 2 GiB
// because it describes a bundle whose writer is not known.  A checkpoint's
// journal is as large as the dirty set, and the dirty set is bounded by the
// store's hard limit: the 16 GiB / 12 GiB acceptance produced a 9.1 GiB
// journal.  Every reader of such a bundle must accept what its own writer
// produced, so this raises the journal bound to twice the dirty set plus
// overhead when a budget was carried.  A zero budget keeps the historical
// floor -- that is what an import, whose capacity is discovered after launch,
// still uses.
//
// Measured cost of not applying it everywhere: the M3 provider accepted the
// 9.1 GiB checkpoint, and the next `disk start` refused its own bundle at
// `recover_complete_image: journal bound readback=9111699456
// limit=2147483648 file=9111707648`.
PersistenceLimits recovery_persistence_limits(
    std::uint64_t virtual_bytes, std::uint64_t ram_limit_bytes) noexcept;

} // namespace myrd::persistence
