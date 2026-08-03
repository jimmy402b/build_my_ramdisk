#pragma once

#include "src/common/operation_context.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace myrd {

enum class BlockLoadKind {
    Data,
    AllZero
};

struct BlockLoadResult {
    BlockLoadKind kind;
};

class IBackingStore {
public:
    virtual ~IBackingStore() = default;

    virtual Result<BlockLoadResult> load_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const = 0;
};

} // namespace myrd
