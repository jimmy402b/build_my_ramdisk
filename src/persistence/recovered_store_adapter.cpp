#include "recovered_store_adapter.h"

#include "src/persistence/composite_backing_store.h"
#include "src/persistence/persistence_types.h"

#include <utility>

namespace myrd::persistence {
namespace {

Error adapter_error(const wchar_t* message) {
    return {
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat),
        L"open_recovered_dynamic_store",
        message};
}

bool valid_recovered_view(const RecoveredImage& image) noexcept {
    const auto composite = std::dynamic_pointer_cast<
        const CompositeBackingStore>(image.backing_view.store);
    return composite != nullptr &&
        composite->identity() == image.backing_identity &&
        validate_geometry(image.geometry) == GeometryStatus::Ok &&
        image.backing_view.store != nullptr &&
        image.backing_view.generation == image.checkpoint_generation &&
        image.backing_identity.checkpoint_generation ==
            image.checkpoint_generation &&
        image.backing_identity.base_generation == image.base_generation &&
        image.backing_identity.image_uuid == image.image_uuid &&
        image.backing_identity.geometry.virtual_bytes ==
            image.geometry.virtual_bytes &&
        image.backing_identity.geometry.logical_sector_size ==
            image.geometry.logical_sector_size &&
        image.backing_identity.geometry.physical_sector_size ==
            image.geometry.physical_sector_size;
}

}  // namespace

Result<std::unique_ptr<DynamicBlockStore>> open_recovered_dynamic_store(
    const RecoveredImage& image,
    DynamicBlockStoreOptions options) {
    if (!valid_recovered_view(image)) {
        return Result<std::unique_ptr<DynamicBlockStore>>::failure(
            adapter_error(L"recovered image has an invalid backing view"));
    }
    DynamicBlockStoreCreateArgs args{};
    args.logical_size_bytes = image.geometry.virtual_bytes;
    args.sector_size_bytes = image.geometry.logical_sector_size;
    args.options = options;
    args.initial_state = InitialBlockState::BackingUnresident;
    args.backing_store = image.backing_view.store;
    args.initial_backing_generation = image.backing_view.generation;
    return DynamicBlockStore::create(std::move(args));
}

Result<void> install_recovered_backing_view(
    DynamicBlockStore& store,
    const RecoveredImage& image) {
    if (!valid_recovered_view(image) ||
        image.geometry.virtual_bytes != store.size_bytes() ||
        image.geometry.logical_sector_size != store.sector_size() ||
        image.backing_view.generation != image.checkpoint_generation) {
        return Result<void>::failure(adapter_error(
            L"recovered backing view does not match the dynamic store"));
    }
    return store.install_backing_view(image.backing_view);
}

}  // namespace myrd::persistence
