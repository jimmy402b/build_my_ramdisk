#include "zero_backing_store.h"

#include "block_constants.h"

namespace myrd {

Result<BlockLoadResult> ZeroBackingStore::load_block(
    const OperationContext& context,
    std::uint64_t /*block_index*/,
    std::span<std::byte> destination) const {
    if (destination.size() != kBlockSizeBytes) {
        return Result<BlockLoadResult>::failure({
            ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"ZeroBackingStore::load_block",
            L"destination must be exactly one block"});
    }

    const auto live = check_operation_context(context);
    if (!live) {
        return Result<BlockLoadResult>::failure(live.error());
    }

    return Result<BlockLoadResult>::success({BlockLoadKind::AllZero});
}

} // namespace myrd
