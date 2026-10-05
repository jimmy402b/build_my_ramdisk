#include "m4_persistence_adapter.h"

#include "src/common/sha256.h"
#include "src/m3/m3_provider.h"
#include "src/m3/m3_windows_acceptance.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/production_policy.h"
#include "src/persistence/vhdx_format.h"

#include <bcrypt.h>

#include <algorithm>
#include <limits>

namespace myrd {
namespace {

using namespace persistence;

Error adapter_error(const ErrorDomain domain, const std::uint32_t code,
                    const wchar_t* operation, const wchar_t* message) {
    return {domain, code, operation, message, 0};
}

Result<void> validate_options(const M4PersistenceOptions& options) {
    if (options.bundle_root.empty()) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::usage, 2, L"M4PersistenceAdapter",
            L"bundle root is required"));
    }
    const auto status = validate_geometry(
        ImageGeometry{options.virtual_bytes, 512, 4096});
    if (status != GeometryStatus::Ok) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::usage, static_cast<std::uint32_t>(status),
            L"M4PersistenceAdapter", L"virtual image geometry is invalid"));
    }
    if (options.manifest_name.empty()) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::usage, 2, L"M4PersistenceAdapter",
            L"manifest name is required"));
    }
    return Result<void>::success();
}

template <class T>
Result<T> random_bytes(T& bytes, NTSTATUS& status) {
    status = BCryptGenRandom(
        nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
        static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        return Result<T>::failure(adapter_error(
            ErrorDomain::ntstatus, static_cast<std::uint32_t>(status),
            L"M4PersistenceAdapter::create_generation_zero",
            L"BCryptGenRandom failed"));
    }
    return Result<T>::success(bytes);
}

Result<RecoveredImage> make_generation_zero_request(
    const OperationContext& context, const M4PersistenceOptions& options) {
    auto valid = validate_options(options);
    if (!valid.ok()) return Result<RecoveredImage>::failure(valid.error());

    GenerationZeroCreateRequest request{};
    request.bundle_root = options.bundle_root;
    request.manifest_relative_name = options.manifest_name;
    request.base_temporary_name = "base-0000.create.tmp";
    request.base_relative_name = "base-0000.vhdx";
    request.journal_relative_name = "journal-e0001.rdj";
    request.geometry = {options.virtual_bytes, 512, 4096};

    NTSTATUS random_status{};
    // A caller that already knows which image this bundle is supposed to hold
    // supplies it; the generation-zero request then *is* that image rather than
    // one the service would afterwards refuse to record.  Only the identities
    // nobody can predict are generated here.
    if (options.image_uuid.has_value()) {
        request.image_uuid = *options.image_uuid;
    }
    if ((!options.image_uuid.has_value() &&
         !random_bytes(request.image_uuid.bytes, random_status).ok()) ||
        !random_bytes(request.base_identity.file_identity.file_write_guid,
                      random_status).ok() ||
        !random_bytes(request.base_identity.file_identity.data_write_guid,
                      random_status).ok() ||
        !random_bytes(request.base_identity.virtual_disk_id, random_status).ok() ||
        !random_bytes(request.journal_identity.journal_uuid, random_status).ok()) {
        return Result<RecoveredImage>::failure(adapter_error(
            ErrorDomain::ntstatus, static_cast<std::uint32_t>(random_status),
            L"M4PersistenceAdapter::create_generation_zero",
            L"could not generate production image identities"));
    }
    request.journal_epoch = 1;
    request.operation_id = 1;
    request.created_time_100ns = 1;
    const auto payload_base = options.virtual_bytes / kVhdxDataBlockBytes;
    if (payload_base == (std::numeric_limits<std::uint64_t>::max)()) {
        return Result<RecoveredImage>::failure(adapter_error(
            ErrorDomain::usage, 2, L"M4PersistenceAdapter::create_generation_zero",
            L"VHDX payload count overflow"));
    }
    const auto payloads = payload_base + 1;
    request.vhdx_limits = {payloads, payloads, kVhdxDataBlockBytes};
    request.recovery.limits = production_persistence_limits(options.virtual_bytes);
    return create_generation_zero_image(context, request);
}

} // namespace

M4PersistenceAdapter::M4PersistenceAdapter(M4PersistenceOptions options) noexcept
    : options_(std::move(options)) {}

Result<persistence::RecoveredImage>
M4PersistenceAdapter::create_generation_zero(
    const OperationContext& context) const {
    return make_generation_zero_request(context, options_);
}

Result<persistence::RecoveredImage>
M4PersistenceAdapter::recover_complete(const OperationContext& context) const {
    auto valid = validate_options(options_);
    if (!valid.ok()) return Result<persistence::RecoveredImage>::failure(valid.error());
    auto outcome = persistence::recover_complete_image(
        context, options_.bundle_root, options_.manifest_name,
        {persistence::production_persistence_limits(options_.virtual_bytes)});
    if (!outcome.ok() || !outcome.image.has_value()) {
        return Result<persistence::RecoveredImage>::failure(outcome.error.value_or(
            adapter_error(ErrorDomain::persistence,
                          static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
                          L"M4PersistenceAdapter::recover_complete",
                          L"recovery did not select a complete image")));
    }
    if (outcome.image->geometry.virtual_bytes != options_.virtual_bytes) {
        return Result<persistence::RecoveredImage>::failure(adapter_error(
            ErrorDomain::usage, 2, L"M4PersistenceAdapter::recover_complete",
            L"recovered image geometry does not match requested size"));
    }
    return Result<persistence::RecoveredImage>::success(*outcome.image);
}

m3::M3LifecycleOutcome M4PersistenceAdapter::compact_offline(
    const OperationContext& context) const {
    if (options_.bundle_root.empty() || options_.manifest_name.empty()) {
        m3::M3LifecycleOutcome result{};
        result.exit_code = 1;
        result.error = adapter_error(ErrorDomain::usage, 2,
                                     L"M4PersistenceAdapter::compact_offline",
                                     L"bundle root and manifest name are required");
        return result;
    }
    constexpr std::uint64_t kTiB = 1024ULL * 1024ULL * 1024ULL * 1024ULL;
    const auto discovered = persistence::recover_complete_image(
        context, options_.bundle_root, options_.manifest_name,
        {persistence::production_persistence_limits(kTiB)});
    if (!discovered.ok() || !discovered.image.has_value()) {
        m3::M3LifecycleOutcome result{};
        result.exit_code = 1;
        result.error = discovered.error.value_or(adapter_error(
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::VerificationFailed),
            L"M4PersistenceAdapter::compact_offline",
            L"recovery did not select an image for compaction"));
        return result;
    }
    if (discovered.image->checkpoint_generation == 0) {
        m3::M3LifecycleOutcome result{};
        result.exit_code = 1;
        result.error = adapter_error(
            ErrorDomain::usage, 2, L"M4PersistenceAdapter::compact_offline",
            L"generation-zero bundle cannot be compacted");
        return result;
    }
    m3::M3CliConfig config{};
    config.command = m3::M3CliCommand::Compact;
    config.bundle_root = options_.bundle_root;
    config.manifest_name = options_.manifest_name;
    config.persistence_limits = persistence::production_persistence_limits(
        discovered.image->geometry.virtual_bytes);
    config.store_options = m3::production_store_options();
    config.barrier = persistence::VolumeBarrierKind::RawBlockTestNoFilesystemCache;
    return m3::run_m3_lifecycle(context, config);
}

Result<persistence::RecoveredImage> m4_create_generation_zero(
    const OperationContext& context, const std::filesystem::path& bundle_root,
    const std::uint64_t virtual_bytes) {
    return M4PersistenceAdapter({bundle_root, virtual_bytes})
        .create_generation_zero(context);
}

Result<persistence::RecoveredImage> m4_recover_complete(
    const OperationContext& context, const std::filesystem::path& bundle_root,
    const std::uint64_t virtual_bytes) {
    return M4PersistenceAdapter({bundle_root, virtual_bytes})
        .recover_complete(context);
}

m3::M3LifecycleOutcome m4_compact_offline(
    const OperationContext& context,
    const std::filesystem::path& bundle_root) {
    return M4PersistenceAdapter({bundle_root, 0})
        .compact_offline(context);
}

} // namespace myrd
