#pragma once

#include "src/common/result.h"
#include "src/persistence/recovery_engine.h"
#include "src/storage/dynamic_block_store.h"

#include <memory>

namespace myrd::persistence {

// Composition-root adapter: persistence owns the immutable recovered view;
// M2 owns the memory/eviction state. No file format or path crosses this
// boundary, only the shared IBackingStore and its generation identity.
[[nodiscard]] Result<std::unique_ptr<DynamicBlockStore>>
open_recovered_dynamic_store(
    const RecoveredImage& image,
    DynamicBlockStoreOptions options);

// Used by later checkpoint/recovery publication. The store itself remains the
// sole owner of the install lifetime boundary and generation ordering.
[[nodiscard]] Result<void> install_recovered_backing_view(
    DynamicBlockStore& store,
    const RecoveredImage& image);

}  // namespace myrd::persistence
