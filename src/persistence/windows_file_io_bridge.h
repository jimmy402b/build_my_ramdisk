#pragma once

#include "src/persistence/file_io.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <memory>

namespace myrd::persistence::detail {

// Internal Task 12 bridge only. The caller must already own a validated
// capability; this duplicates that handle so exact I/O stays attached to the
// same opened file object instead of reopening a diagnostic path.
[[nodiscard]] Result<std::unique_ptr<IPersistenceFile>>
duplicate_windows_persistence_file(HANDLE validated_handle);

} // namespace myrd::persistence::detail
