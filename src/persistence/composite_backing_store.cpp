#include "src/persistence/composite_backing_store.h"

#include "src/persistence/journal_reader.h"
#include "src/persistence/vhdx_reader.h"

#include <atomic>
#include <new>
#include <optional>
#include <utility>

namespace myrd::persistence {
namespace {

template <typename T>
Result<T> invalid_argument(const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat),
        L"CompositeBackingStore::create",
        message});
}

Result<BlockLoadResult> invalid_request(const wchar_t* message) {
    return Result<BlockLoadResult>::failure(Error{
        ErrorDomain::storage,
        static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
        L"CompositeBackingStore::load_block",
        message});
}

bool valid_identity(const CompositeBackingIdentity& identity) noexcept {
    return identity.schema_version == kCompositeBackingIdentitySchema &&
           !identity.image_uuid.is_nil() &&
           validate_geometry(identity.geometry) == GeometryStatus::Ok &&
           identity.checkpoint_generation != kInvalidGeneration &&
           identity.base_generation != kInvalidGeneration &&
           identity.journal_epoch != kInvalidGeneration &&
           identity.base_generation <= identity.checkpoint_generation &&
           identity.journal_root_generation ==
               identity.checkpoint_generation &&
           identity.base_expected_length != 0;
}

class JournalReaderLayer final : public detail::ICompositeJournalLayer {
public:
    explicit JournalReaderLayer(std::unique_ptr<JournalReader> reader)
        : reader_(std::move(reader)) {}

    Result<detail::CompositeJournalRead> read_block(
        const OperationContext& context,
        const std::uint64_t block_index,
        const std::span<std::byte> destination) const override {
        auto result = reader_->read_block(context, block_index, destination);
        if (!result.ok()) {
            return Result<detail::CompositeJournalRead>::failure(result.error());
        }
        if (!result.value().has_value()) {
            return Result<detail::CompositeJournalRead>::success(
                {detail::CompositeJournalKind::Absent});
        }
        const auto kind = result.value()->kind == BlockLoadKind::Data
            ? detail::CompositeJournalKind::Data
            : detail::CompositeJournalKind::Zero;
        return Result<detail::CompositeJournalRead>::success({kind});
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        const std::uint64_t first_block) const override {
        return reader_->next_indexed_block(context, first_block);
    }

private:
    std::unique_ptr<JournalReader> reader_;
};

class VhdxReaderLayer final : public detail::ICompositeBaseLayer {
public:
    explicit VhdxReaderLayer(std::unique_ptr<VhdxReader> reader)
        : reader_(std::move(reader)) {}

    Result<detail::CompositeBaseRead> read_block(
        const OperationContext& context,
        const std::uint64_t block_index,
        const std::span<std::byte> destination) const override {
        auto result = reader_->read_block(context, block_index, destination);
        if (!result.ok()) {
            return Result<detail::CompositeBaseRead>::failure(result.error());
        }
        const auto kind = result.value().kind == BlockLoadKind::Data
            ? detail::CompositeBaseKind::Data
            : detail::CompositeBaseKind::Zero;
        return Result<detail::CompositeBaseRead>::success({kind});
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        const std::uint64_t first_block) const override {
        return reader_->next_allocated_block(context, first_block);
    }

private:
    std::unique_ptr<VhdxReader> reader_;
};

}  // namespace

bool CompositeBackingIdentity::operator==(
    const CompositeBackingIdentity& other) const noexcept {
    return schema_version == other.schema_version &&
           image_uuid == other.image_uuid &&
           geometry.virtual_bytes == other.geometry.virtual_bytes &&
           geometry.logical_sector_size == other.geometry.logical_sector_size &&
           geometry.physical_sector_size == other.geometry.physical_sector_size &&
           checkpoint_generation == other.checkpoint_generation &&
           base_generation == other.base_generation &&
           base_windows_identity == other.base_windows_identity &&
           base_vhdx_identity == other.base_vhdx_identity &&
           virtual_disk_id == other.virtual_disk_id &&
           base_expected_length == other.base_expected_length &&
           journal_windows_identity == other.journal_windows_identity &&
           journal_identity == other.journal_identity &&
           journal_epoch == other.journal_epoch &&
           journal_root_generation == other.journal_root_generation &&
           journal_committed_length == other.journal_committed_length &&
           commit == other.commit && root == other.root &&
           feature_flags == other.feature_flags;
}

struct CompositeBackingStore::Impl final {
    CompositeBackingIdentity identity;
    std::unique_ptr<detail::ICompositeJournalLayer> journal;
    std::unique_ptr<detail::ICompositeBaseLayer> base;
    mutable std::atomic<std::uint64_t> load_calls{};
    mutable std::atomic<std::uint64_t> journal_read_calls{};
    mutable std::atomic<std::uint64_t> base_read_calls{};
    mutable std::atomic<std::uint64_t> fallback_calls{};
};

CompositeBackingStore::CompositeBackingStore(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

CompositeBackingStore::~CompositeBackingStore() = default;

Result<std::unique_ptr<CompositeBackingStore>>
CompositeBackingStore::create(
    CompositeBackingIdentity identity,
    std::unique_ptr<JournalReader> journal,
    std::unique_ptr<VhdxReader> base) {
    if (!journal || !base) {
        return invalid_argument<std::unique_ptr<CompositeBackingStore>>(
            L"journal and base readers are required");
    }
    try {
        return create_layers(
            std::move(identity),
            std::make_unique<JournalReaderLayer>(std::move(journal)),
            std::make_unique<VhdxReaderLayer>(std::move(base)));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<CompositeBackingStore>>::failure(Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(
                PersistenceErrorCode::FatalInternalInvariant),
            L"CompositeBackingStore::create",
            L"allocation failed"});
    }
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<std::unique_ptr<CompositeBackingStore>>
CompositeBackingStore::create_for_testing(
    CompositeBackingIdentity identity,
    std::unique_ptr<detail::ICompositeJournalLayer> journal,
    std::unique_ptr<detail::ICompositeBaseLayer> base) {
    return create_layers(
        std::move(identity), std::move(journal), std::move(base));
}
#endif

Result<std::unique_ptr<CompositeBackingStore>>
CompositeBackingStore::create_layers(
    CompositeBackingIdentity identity,
    std::unique_ptr<detail::ICompositeJournalLayer> journal,
    std::unique_ptr<detail::ICompositeBaseLayer> base) {
    if (!valid_identity(identity)) {
        return invalid_argument<std::unique_ptr<CompositeBackingStore>>(
            L"composite backing identity is invalid");
    }
    if (!journal || !base) {
        return invalid_argument<std::unique_ptr<CompositeBackingStore>>(
            L"journal and base layers are required");
    }
    try {
        auto impl = std::make_unique<Impl>();
        impl->identity = std::move(identity);
        impl->journal = std::move(journal);
        impl->base = std::move(base);
        return Result<std::unique_ptr<CompositeBackingStore>>::success(
            std::unique_ptr<CompositeBackingStore>(
                new CompositeBackingStore(std::move(impl))));
    } catch (const std::bad_alloc&) {
        return Result<std::unique_ptr<CompositeBackingStore>>::failure(Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(
                PersistenceErrorCode::FatalInternalInvariant),
            L"CompositeBackingStore::create",
            L"allocation failed"});
    }
}

Result<BlockLoadResult> CompositeBackingStore::load_block(
    const OperationContext& context,
    const std::uint64_t block_index,
    const std::span<std::byte> destination) const {
    if (destination.size() != kM2BlockSize) {
        return invalid_request(L"destination must be exactly one M2 block");
    }
    if (block_index > (impl_->identity.geometry.virtual_bytes - 1) /
                          kM2BlockSize) {
        return invalid_request(L"block index exceeds the virtual disk");
    }
    auto context_status = check_operation_context(context);
    if (!context_status.ok()) {
        return Result<BlockLoadResult>::failure(context_status.error());
    }

    impl_->load_calls.fetch_add(1, std::memory_order_relaxed);
    impl_->journal_read_calls.fetch_add(1, std::memory_order_relaxed);
    auto journal = impl_->journal->read_block(
        context, block_index, destination);
    if (!journal.ok()) {
        return Result<BlockLoadResult>::failure(journal.error());
    }

    switch (journal.value().kind) {
    case detail::CompositeJournalKind::Data:
        return Result<BlockLoadResult>::success({BlockLoadKind::Data});
    case detail::CompositeJournalKind::Zero:
        return Result<BlockLoadResult>::success({BlockLoadKind::AllZero});
    case detail::CompositeJournalKind::Discard:
        return Result<BlockLoadResult>::success({BlockLoadKind::AllZero});
    case detail::CompositeJournalKind::Absent:
        break;
    }

    context_status = check_operation_context(context);
    if (!context_status.ok()) {
        return Result<BlockLoadResult>::failure(context_status.error());
    }
    impl_->fallback_calls.fetch_add(1, std::memory_order_relaxed);
    impl_->base_read_calls.fetch_add(1, std::memory_order_relaxed);
    auto base = impl_->base->read_block(context, block_index, destination);
    if (!base.ok()) {
        return Result<BlockLoadResult>::failure(base.error());
    }
    if (base.value().kind == detail::CompositeBaseKind::Data) {
        return Result<BlockLoadResult>::success({BlockLoadKind::Data});
    }
    return Result<BlockLoadResult>::success({BlockLoadKind::AllZero});
}

Result<void> CompositeBackingStore::enumerate_candidate_blocks(
    const OperationContext& context,
    const CompositeCandidateConsumer& consume) const {
    constexpr auto operation =
        L"CompositeBackingStore::enumerate_candidate_blocks";
    if (!consume) {
        return Result<void>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat),
            operation, L"candidate consumer is required"});
    }
    const auto last_block =
        (impl_->identity.geometry.virtual_bytes - 1) / kM2BlockSize;
    std::uint64_t journal_first{};
    std::uint64_t base_first{};
    std::optional<std::uint64_t> journal_candidate;
    std::optional<std::uint64_t> base_candidate;
    bool journal_loaded{};
    bool base_loaded{};
    bool journal_exhausted{};
    bool base_exhausted{};

    const auto invalid = [last_block](
        const std::uint64_t first,
        const std::optional<std::uint64_t>& candidate) {
        return candidate.has_value() &&
               (candidate.value() < first ||
                candidate.value() > last_block);
    };

    while (!journal_exhausted || !base_exhausted) {
        auto live = check_operation_context(context);
        if (!live.ok()) return live;
        if (!journal_loaded && !journal_exhausted) {
            auto next = impl_->journal->next_candidate_block(
                context, journal_first);
            if (!next.ok()) return Result<void>::failure(next.error());
            if (invalid(journal_first, next.value())) {
                return Result<void>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(
                        PersistenceErrorCode::BackingDataCorrupt),
                    operation,
                    L"journal candidate is out of order or range"});
            }
            journal_candidate = next.value();
            journal_loaded = journal_candidate.has_value();
            journal_exhausted = !journal_loaded;
        }
        if (!base_loaded && !base_exhausted) {
            auto next = impl_->base->next_candidate_block(
                context, base_first);
            if (!next.ok()) return Result<void>::failure(next.error());
            if (invalid(base_first, next.value())) {
                return Result<void>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(
                        PersistenceErrorCode::BackingDataCorrupt),
                    operation, L"base candidate is out of order or range"});
            }
            base_candidate = next.value();
            base_loaded = base_candidate.has_value();
            base_exhausted = !base_loaded;
        }
        live = check_operation_context(context);
        if (!live.ok()) return live;
        if (!journal_loaded && !base_loaded) break;

        const auto selected = !journal_loaded
            ? base_candidate.value()
            : !base_loaded
                ? journal_candidate.value()
                : (std::min)(journal_candidate.value(),
                             base_candidate.value());
        auto consumed = consume(selected);
        if (!consumed.ok()) return consumed;
        live = check_operation_context(context);
        if (!live.ok()) return live;

        if (journal_loaded && journal_candidate.value() == selected) {
            journal_loaded = false;
            if (selected == last_block) {
                journal_exhausted = true;
            } else {
                journal_first = selected + 1;
            }
        }
        if (base_loaded && base_candidate.value() == selected) {
            base_loaded = false;
            if (selected == last_block) {
                base_exhausted = true;
            } else {
                base_first = selected + 1;
            }
        }
    }
    return check_operation_context(context);
}

CompositeBackingIdentity CompositeBackingStore::identity() const noexcept {
    return impl_->identity;
}

CompositeBackingStats CompositeBackingStore::stats() const noexcept {
    return {
        impl_->load_calls.load(std::memory_order_relaxed),
        impl_->journal_read_calls.load(std::memory_order_relaxed),
        impl_->base_read_calls.load(std::memory_order_relaxed),
        impl_->fallback_calls.load(std::memory_order_relaxed)};
}

}  // namespace myrd::persistence
