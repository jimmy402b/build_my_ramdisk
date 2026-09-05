#pragma once

#include "src/persistence/persistence_types.h"

#include <cstdint>

namespace myrd::persistence {

// One finite recovery/inspection policy shared by product workers, recovery
// adapters and acceptance executables.  Callers must not duplicate these
// bounds at a composition root.
PersistenceLimits production_persistence_limits(
    std::uint64_t virtual_bytes) noexcept;

} // namespace myrd::persistence
