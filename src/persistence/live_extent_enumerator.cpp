#include "src/persistence/live_extent_enumerator.h"

#include "src/common/crc32c.h"
#include "src/common/sha256_stream.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace myrd::persistence {
namespace {

template <class T>
Result<T> extent_failure(const PersistenceErrorCode code,
                         const wchar_t* operation,
                         const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        operation,
        message});
}

bool same_geometry(const ImageGeometry& left,
                   const ImageGeometry& right) noexcept {
    return left.virtual_bytes == right.virtual_bytes &&
           left.logical_sector_size == right.logical_sector_size &&
           left.physical_sector_size == right.physical_sector_size;
}

bool valid_extent_shape(const LiveDataExtent& extent,
                        const CompositeBackingIdentity& identity,
                        const BackingGeneration backing_generation,
                        const std::uint64_t max_extent_bytes) noexcept {
    if (extent.image_uuid != identity.image_uuid ||
        extent.checkpoint_generation != identity.checkpoint_generation ||
        extent.backing_generation != backing_generation ||
        extent.logical_offset % kM2BlockSize != 0 ||
        extent.length == 0 || extent.length > max_extent_bytes ||
        extent.logical_offset >= identity.geometry.virtual_bytes ||
        extent.length > identity.geometry.virtual_bytes -
                            extent.logical_offset) {
        return false;
    }
    const auto end = extent.logical_offset + extent.length;
    return extent.length % kM2BlockSize == 0 ||
           end == identity.geometry.virtual_bytes;
}

}  // namespace

struct LiveExtentEnumerator::Impl final {
    BackingView view{};
    const CompositeBackingStore* composite{};
    CompositeBackingIdentity identity{};
    LiveExtentEnumeratorOptions options{};
};

LiveExtentEnumerator::LiveExtentEnumerator(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

LiveExtentEnumerator::~LiveExtentEnumerator() = default;

Result<std::unique_ptr<LiveExtentEnumerator>> LiveExtentEnumerator::create(
    BackingView captured_view,
    CompositeBackingIdentity identity,
    const LiveExtentEnumeratorOptions options) {
    constexpr auto operation = L"LiveExtentEnumerator::create";
    if (!captured_view.store || options.max_extent_bytes < kM2BlockSize ||
        options.max_extent_bytes % kM2BlockSize != 0 ||
        options.max_extent_bytes >
            static_cast<std::uint64_t>(
                (std::numeric_limits<std::size_t>::max)()) ||
        validate_geometry(identity.geometry) != GeometryStatus::Ok ||
        captured_view.generation != identity.checkpoint_generation) {
        return extent_failure<std::unique_ptr<LiveExtentEnumerator>>(
            PersistenceErrorCode::InvalidFormat, operation,
            L"captured view, identity or extent bound is invalid");
    }
    const auto* composite = dynamic_cast<const CompositeBackingStore*>(
        captured_view.store.get());
    if (composite == nullptr || composite->identity() != identity ||
        !same_geometry(composite->identity().geometry, identity.geometry)) {
        return extent_failure<std::unique_ptr<LiveExtentEnumerator>>(
            PersistenceErrorCode::GenerationConflict, operation,
            L"captured store identity does not match the frozen view");
    }
    try {
        auto impl = std::make_unique<Impl>();
        impl->view = std::move(captured_view);
        impl->composite = composite;
        impl->identity = std::move(identity);
        impl->options = options;
        return Result<std::unique_ptr<LiveExtentEnumerator>>::success(
            std::unique_ptr<LiveExtentEnumerator>(
                new LiveExtentEnumerator(std::move(impl))));
    } catch (const std::bad_alloc&) {
        return extent_failure<std::unique_ptr<LiveExtentEnumerator>>(
            PersistenceErrorCode::SnapshotResourceExhausted, operation,
            L"allocating live extent enumerator failed");
    }
}

Result<LiveExtentEnumerationMetrics> LiveExtentEnumerator::enumerate(
    const OperationContext& context,
    const LiveExtentConsumer& consume) const {
    constexpr auto operation = L"LiveExtentEnumerator::enumerate";
    if (!consume) {
        return extent_failure<LiveExtentEnumerationMetrics>(
            PersistenceErrorCode::InvalidFormat, operation,
            L"synchronous extent consumer is required");
    }

    struct Pending final {
        std::uint64_t offset{};
        std::uint64_t length{};
        std::unique_ptr<Sha256Stream> sha{};
        std::unique_ptr<Crc32cStream> crc{};
    };

    try {
        LiveExtentEnumerationMetrics metrics{};
        metrics.peak_scratch_bytes = kM2BlockSize + sizeof(Pending);
        std::array<std::byte, kM2BlockSize> scratch{};
        Pending pending{};

        const auto flush_pending = [&]() -> Result<void> {
            if (pending.length == 0) return Result<void>::success();
            auto digest = pending.sha->finalize_digest();
            if (!digest.ok()) return Result<void>::failure(digest.error());
            auto crc = pending.crc->finalize();
            if (!crc.ok()) return Result<void>::failure(crc.error());
            const LiveDataExtent extent{
                impl_->identity.image_uuid,
                impl_->identity.checkpoint_generation,
                impl_->view.generation,
                pending.offset,
                pending.length,
                crc.value(),
                digest.value()};
            auto live = check_operation_context(context);
            if (!live.ok()) return live;
            auto consumed = consume(extent);
            if (!consumed.ok()) return consumed;
            live = check_operation_context(context);
            if (!live.ok()) return live;
            ++metrics.extents_emitted;
            pending = Pending{};
            return Result<void>::success();
        };

        const auto virtual_bytes = impl_->identity.geometry.virtual_bytes;
        auto candidates = impl_->composite->enumerate_candidate_blocks(
            context, [&](const std::uint64_t block) -> Result<void> {
            scratch.fill(std::byte{0});
            auto loaded = impl_->view.store->load_block(
                context, block, scratch);
            if (!loaded.ok()) {
                return Result<void>::failure(loaded.error());
            }
            auto live = check_operation_context(context);
            if (!live.ok()) return live;
            ++metrics.candidate_blocks_examined;
            const auto offset = block * kM2BlockSize;
            const auto valid_bytes = (std::min)(
                kM2BlockSize, virtual_bytes - offset);
            const auto valid = std::span<const std::byte>{scratch}.first(
                static_cast<std::size_t>(valid_bytes));
            const bool logical_zero =
                loaded.value().kind == BlockLoadKind::AllZero ||
                std::all_of(valid.begin(), valid.end(),
                            [](const std::byte value) {
                                return value == std::byte{0};
                            });
            if (logical_zero) {
                auto flushed = flush_pending();
                if (!flushed.ok()) return flushed;
                ++metrics.zero_blocks;
                return Result<void>::success();
            }

            if (pending.length != 0 &&
                (offset != pending.offset + pending.length ||
                 valid_bytes > impl_->options.max_extent_bytes -
                                   pending.length)) {
                auto flushed = flush_pending();
                if (!flushed.ok()) return flushed;
            }
            if (pending.length == 0) {
                pending.offset = offset;
                pending.sha = std::make_unique<Sha256Stream>();
                pending.crc = std::make_unique<Crc32cStream>();
            }
            auto sha_updated = pending.sha->update(valid);
            if (!sha_updated.ok()) return sha_updated;
            auto crc_updated = pending.crc->update(valid);
            if (!crc_updated.ok()) return crc_updated;
            pending.length += valid_bytes;
            ++metrics.data_blocks;
            metrics.bytes_hashed += valid_bytes;
            return Result<void>::success();
        });
        if (!candidates.ok()) {
            return Result<LiveExtentEnumerationMetrics>::failure(
                candidates.error());
        }
        auto flushed = flush_pending();
        if (!flushed.ok()) {
            return Result<LiveExtentEnumerationMetrics>::failure(
                flushed.error());
        }
        auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<LiveExtentEnumerationMetrics>::failure(
                live.error());
        }
        return Result<LiveExtentEnumerationMetrics>::success(metrics);
    } catch (const std::bad_alloc&) {
        return extent_failure<LiveExtentEnumerationMetrics>(
            PersistenceErrorCode::SnapshotResourceExhausted, operation,
            L"bounded extent scratch allocation failed");
    } catch (...) {
        return extent_failure<LiveExtentEnumerationMetrics>(
            PersistenceErrorCode::FatalInternalInvariant, operation,
            L"extent consumer threw an exception");
    }
}

Result<void> LiveExtentEnumerator::read_verified_extent(
    const OperationContext& context,
    const LiveDataExtent& extent,
    const std::span<std::byte> destination) const {
    constexpr auto operation = L"LiveExtentEnumerator::read_verified_extent";
    if (!valid_extent_shape(
            extent, impl_->identity, impl_->view.generation,
            impl_->options.max_extent_bytes) ||
        extent.length != destination.size()) {
        return extent_failure<void>(
            PersistenceErrorCode::InvalidFormat, operation,
            L"extent descriptor or destination is invalid");
    }

    try {
        std::array<std::byte, kM2BlockSize> scratch{};
        Sha256Stream sha{};
        Crc32cStream crc{};
        std::uint64_t copied{};
        while (copied < extent.length) {
            auto live = check_operation_context(context);
            if (!live.ok()) return Result<void>::failure(live.error());
            const auto logical_offset = extent.logical_offset + copied;
            const auto block = logical_offset / kM2BlockSize;
            const auto length = (std::min)(
                kM2BlockSize, extent.length - copied);
            scratch.fill(std::byte{0});
            auto loaded = impl_->view.store->load_block(
                context, block, scratch);
            if (!loaded.ok()) return Result<void>::failure(loaded.error());
            live = check_operation_context(context);
            if (!live.ok()) return live;
            const auto source = std::span<const std::byte>{scratch}.first(
                static_cast<std::size_t>(length));
            if (loaded.value().kind != BlockLoadKind::Data ||
                std::all_of(source.begin(), source.end(),
                            [](const std::byte value) {
                                return value == std::byte{0};
                            })) {
                return extent_failure<void>(
                    PersistenceErrorCode::BackingDataCorrupt, operation,
                    L"captured DATA extent changed to logical zero");
            }
            std::copy(source.begin(), source.end(),
                      destination.begin() +
                          static_cast<std::ptrdiff_t>(copied));
            auto sha_updated = sha.update(source);
            if (!sha_updated.ok()) return sha_updated;
            auto crc_updated = crc.update(source);
            if (!crc_updated.ok()) return crc_updated;
            copied += length;
        }
        auto digest = sha.finalize_digest();
        if (!digest.ok()) return Result<void>::failure(digest.error());
        auto checksum = crc.finalize();
        if (!checksum.ok()) return Result<void>::failure(checksum.error());
        if (digest.value() != extent.digest ||
            checksum.value() != extent.crc32c) {
            return extent_failure<void>(
                PersistenceErrorCode::BackingDataCorrupt, operation,
                L"captured DATA extent integrity changed");
        }
        auto live = check_operation_context(context);
        if (!live.ok()) return live;
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return extent_failure<void>(
            PersistenceErrorCode::SnapshotResourceExhausted, operation,
            L"verified extent scratch allocation failed");
    }
}

const CompositeBackingIdentity& LiveExtentEnumerator::identity() const noexcept {
    return impl_->identity;
}

BackingGeneration LiveExtentEnumerator::backing_generation() const noexcept {
    return impl_->view.generation;
}

}  // namespace myrd::persistence
