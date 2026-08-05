#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/journal_index.h"
#include "src/storage/backing_store.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace myrd::persistence {

struct JournalReaderOptions final {
    JournalIndexView view{};
    ImageGeometry geometry{};
    JournalIndexCacheOptions index_cache{};
};

class JournalReader final {
public:
    static Result<std::unique_ptr<JournalReader>> create(
        JournalReaderOptions options);

    ~JournalReader();
    JournalReader(JournalReader&&) noexcept;
    JournalReader& operator=(JournalReader&&) noexcept;
    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;

    // nullopt means this immutable journal root does not contain the block.
    // DATA fills destination only after all record and descriptor checks pass;
    // ZERO/DISCARD return AllZero without changing destination.
    Result<std::optional<BlockLoadResult>> read_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const;

private:
    struct Impl;
    explicit JournalReader(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace myrd::persistence
