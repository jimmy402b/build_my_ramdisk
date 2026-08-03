#pragma once

#include "backing_store.h"

namespace myrd {

class ZeroBackingStore final : public IBackingStore {
public:
    Result<BlockLoadResult> load_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const override;
};

} // namespace myrd
