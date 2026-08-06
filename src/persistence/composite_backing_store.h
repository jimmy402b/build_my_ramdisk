#pragma once

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/persistence/file_identity.h"
#include "src/persistence/persistence_types.h"
#include "src/storage/backing_store.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace myrd::persistence {

class JournalReader;
class VhdxReader;

inline constexpr std::uint32_t kCompositeBackingIdentitySchema = 1;

// Complete immutable identity of one journal/base read view.  It deliberately
// contains both Windows object identities and format identities so a cache or
// replacement view cannot accidentally alias another generation.  Production
// callers must derive this value and both readers from the same validated
// recovery candidate; Task 26's recovery factory owns that validation boundary.
struct CompositeBackingIdentity final {
    std::uint32_t schema_version{kCompositeBackingIdentitySchema};
    ImageUuid image_uuid{};
    ImageGeometry geometry{};
    CheckpointGeneration checkpoint_generation{};
    BaseGeneration base_generation{};
    WindowsFileIdentity base_windows_identity{};
    VhdxFileIdentity base_vhdx_identity{};
    std::array<std::byte, 16> virtual_disk_id{};
    std::uint64_t base_expected_length{};
    WindowsFileIdentity journal_windows_identity{};
    JournalFileIdentity journal_identity{};
    JournalEpoch journal_epoch{};
    CheckpointGeneration journal_root_generation{};
    std::uint64_t journal_committed_length{};
    JournalCommitDescriptor commit{};
    PersistentIndexRootDescriptor root{};
    std::uint64_t feature_flags{};

    [[nodiscard]] bool operator==(
        const CompositeBackingIdentity& other) const noexcept;
};

struct CompositeBackingStats final {
    std::uint64_t load_calls{};
    std::uint64_t journal_read_calls{};
    std::uint64_t base_read_calls{};
    std::uint64_t fallback_calls{};
};

using CompositeCandidateConsumer =
    std::function<Result<void>(std::uint64_t block_index)>;

namespace detail {

enum class CompositeJournalKind : std::uint8_t {
    Data,
    Zero,
    Discard,
    Absent,
};

struct CompositeJournalRead final {
    CompositeJournalKind kind{CompositeJournalKind::Absent};
};

enum class CompositeBaseKind : std::uint8_t {
    Data,
    Zero,
    Absent,
};

struct CompositeBaseRead final {
    CompositeBaseKind kind{CompositeBaseKind::Absent};
};

class ICompositeJournalLayer {
public:
    virtual ~ICompositeJournalLayer() = default;
    virtual Result<CompositeJournalRead> read_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const = 0;
    virtual Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        std::uint64_t first_block) const = 0;
};

class ICompositeBaseLayer {
public:
    virtual ~ICompositeBaseLayer() = default;
    virtual Result<CompositeBaseRead> read_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const = 0;
    virtual Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        std::uint64_t first_block) const = 0;
};

}  // namespace detail

// Journal-first immutable backing view.  Only an explicit journal Absent may
// fall through to the VHDX base.  ZERO, DISCARD and every error are terminal.
class CompositeBackingStore final : public IBackingStore {
public:
    static Result<std::unique_ptr<CompositeBackingStore>> create(
        CompositeBackingIdentity identity,
        std::unique_ptr<JournalReader> journal,
        std::unique_ptr<VhdxReader> base);

#if defined(MYRD_ENABLE_TEST_SEAMS)
    static Result<std::unique_ptr<CompositeBackingStore>> create_for_testing(
        CompositeBackingIdentity identity,
        std::unique_ptr<detail::ICompositeJournalLayer> journal,
        std::unique_ptr<detail::ICompositeBaseLayer> base);
#endif

    ~CompositeBackingStore() override;
    CompositeBackingStore(const CompositeBackingStore&) = delete;
    CompositeBackingStore& operator=(const CompositeBackingStore&) = delete;
    CompositeBackingStore(CompositeBackingStore&&) = delete;
    CompositeBackingStore& operator=(CompositeBackingStore&&) = delete;

    Result<BlockLoadResult> load_block(
        const OperationContext& context,
        std::uint64_t block_index,
        std::span<std::byte> destination) const override;

    // Streams the ascending, deduplicated union of blocks referenced by the
    // frozen journal index and frozen VHDX BAT. Two one-element lookaheads
    // prevent repeated BAT gap scans while keeping memory constant. Journal
    // ZERO/DISCARD remain candidates because they shadow older base DATA.
    Result<void> enumerate_candidate_blocks(
        const OperationContext& context,
        const CompositeCandidateConsumer& consume) const;

    [[nodiscard]] CompositeBackingIdentity identity() const noexcept;
    // Monotonic diagnostic counters.  Each field is atomic, but the returned
    // multi-field sample is deliberately an approximate, non-transactional
    // snapshot while concurrent reads are active.
    [[nodiscard]] CompositeBackingStats stats() const noexcept;

private:
    struct Impl;
    explicit CompositeBackingStore(std::unique_ptr<Impl> impl) noexcept;
    static Result<std::unique_ptr<CompositeBackingStore>> create_layers(
        CompositeBackingIdentity identity,
        std::unique_ptr<detail::ICompositeJournalLayer> journal,
        std::unique_ptr<detail::ICompositeBaseLayer> base);

    std::unique_ptr<Impl> impl_;
};

}  // namespace myrd::persistence
