#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "src/run/runtime_adapter.h"

#include "src/m3/m3_windows_acceptance.h"
#include "src/persistence/production_policy.h"
#include "src/run/driver_preflight.h"
#include "src/run/m4_persistence_adapter.h"

#include <windows.h>

#include <chrono>
#include <iostream>
#include <utility>

namespace myrd {
namespace {

Error adapter_error(const ErrorDomain domain, const std::uint32_t code,
                    const wchar_t* operation, const wchar_t* message,
                    const std::uint32_t native_code = 0) {
    return {domain, code, operation, message, native_code};
}

Result<void> ensure_bundle_directory(const std::filesystem::path& bundle) {
    std::error_code error;
    if (!std::filesystem::create_directories(bundle, error) && error) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::storage, static_cast<std::uint32_t>(error.value()),
            L"create_bundle_directory",
            L"bundle directory could not be created"));
    }
    return Result<void>::success();
}

DynamicBlockStoreOptions store_options_for(const std::uint64_t ram_limit_bytes) {
    auto result = m3::production_store_options();
    // Zero is "the caller has no budget to impose", not "a store with no
    // memory".  Every one of these is a hard limit that must be non-zero and
    // strictly ordered, so a literal zero can only produce options no provider
    // will accept -- which is exactly what it did: the M5 worker passes no
    // budget because the launch spec does not yet carry the profile's memory
    // policy, and every `start()` it ever attempted died in `M3Provider::open`
    // with "provider limits are invalid" before reaching a bundle.
    if (ram_limit_bytes == 0) return result;
    // The parser guarantees at least 64 MiB and power-of-two byte units, so
    // these limits remain block-aligned and satisfy the M3 strict ordering.
    result.hard_limit_bytes = ram_limit_bytes;
    result.soft_limit_bytes = ram_limit_bytes / 2;
    result.eviction_low_watermark_bytes = ram_limit_bytes / 4;
    return result;
}

DiskCheckpointDisposition map_disposition(
    const persistence::CommitDisposition disposition) noexcept {
    switch (disposition) {
    case persistence::CommitDisposition::Committed:
        return DiskCheckpointDisposition::committed;
    case persistence::CommitDisposition::CommitIndeterminate:
        return DiskCheckpointDisposition::commit_indeterminate;
    // Kept apart from the indeterminate case above.  Collapsing them told the
    // service "the outcome is unknown" when the truth was "the bytes landed and
    // only the epoch publication did not" -- a known state with its own recovery
    // path, and one where the safe answer is the opposite of parking.
    case persistence::CommitDisposition::CommittedNeedsReconciliation:
        return DiskCheckpointDisposition::committed_needs_reconciliation;
    case persistence::CommitDisposition::NotCommitted:
        return DiskCheckpointDisposition::not_committed;
    }
    return DiskCheckpointDisposition::commit_indeterminate;
}

// Whether the candidate's bytes actually reached the bundle.  Both landed
// dispositions count: a commit that landed but could not confirm its epoch is
// still a commit, and treating it as un-landed would let a caller restore a
// prior record over bytes that are really there.
bool checkpoint_landed(const DiskCheckpointDisposition disposition) noexcept {
    return disposition == DiskCheckpointDisposition::committed
        || disposition
            == DiskCheckpointDisposition::committed_needs_reconciliation;
}

// Mirrors the persistence context across the engine boundary.  Nothing here is
// defaulted or derived: every field is relayed, including the safe_context the
// service quotes back, because a context reconstructed from its parts would be a
// record of a failure that did not happen.
DiskCheckpointErrorContext map_error_context(
    const persistence::PersistenceErrorContext& context,
    const DiskCheckpointDisposition disposition) {
    DiskCheckpointErrorContext mirrored{};
    mirrored.operation = context.operation;
    mirrored.code = static_cast<std::uint32_t>(context.code);
    mirrored.phase = static_cast<std::uint8_t>(context.phase);
    mirrored.native_code = context.native_code;
    mirrored.native_operation_id = context.operation_id;
    mirrored.image_uuid = context.image_uuid.bytes;
    mirrored.generation = context.generation;
    mirrored.disposition = disposition;
    mirrored.safe_context = context.safe_context;
    return mirrored;
}

} // namespace

Result<DiskBundleKind> classify_bundle(const std::filesystem::path& bundle) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(bundle, error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return Result<DiskBundleKind>::failure(adapter_error(
            ErrorDomain::storage, static_cast<std::uint32_t>(error.value()),
            L"classify_bundle", L"bundle status could not be read"));
    }
    if (!std::filesystem::exists(status)) {
        return Result<DiskBundleKind>::success(DiskBundleKind::empty);
    }
    if (!std::filesystem::is_directory(status)) {
        return Result<DiskBundleKind>::failure(adapter_error(
            ErrorDomain::usage, 1, L"classify_bundle",
            L"bundle is not a directory"));
    }

    error.clear();
    const auto manifest =
        std::filesystem::symlink_status(bundle / L"disk.rdm", error);
    if (error && error != std::errc::no_such_file_or_directory) {
        return Result<DiskBundleKind>::failure(adapter_error(
            ErrorDomain::storage, static_cast<std::uint32_t>(error.value()),
            L"classify_bundle", L"manifest status could not be read"));
    }
    if (!error && std::filesystem::is_regular_file(manifest)) {
        return Result<DiskBundleKind>::success(DiskBundleKind::manifest);
    }

    error.clear();
    std::filesystem::directory_iterator iterator(bundle, error);
    if (error) {
        return Result<DiskBundleKind>::failure(adapter_error(
            ErrorDomain::storage, static_cast<std::uint32_t>(error.value()),
            L"classify_bundle", L"bundle contents could not be read"));
    }
    return iterator == std::filesystem::directory_iterator{}
        ? Result<DiskBundleKind>::success(DiskBundleKind::empty)
        : Result<DiskBundleKind>::success(DiskBundleKind::unknown_nonempty);
}

Result<void> require_elevated_administrator() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::win32, GetLastError(), L"require_elevation",
            L"OpenProcessToken failed", GetLastError()));
    }

    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    const BOOL elevation_ok = GetTokenInformation(
        token, TokenElevation, &elevation, sizeof(elevation), &returned);
    const DWORD elevation_error = GetLastError();
    CloseHandle(token);
    if (!elevation_ok || returned != sizeof(elevation) ||
        !elevation.TokenIsElevated) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::usage, 1, L"require_elevation",
            L"run requires an elevated administrator token", elevation_error));
    }
    return Result<void>::success();
}

RuntimeAdapterEnvironment make_production_environment() {
    RuntimeAdapterEnvironment environment{};
    // This gate runs ahead of every bundle status/open/create operation, so a
    // missing or disabled driver leaves the bundle untouched, including a
    // not-yet-created bundle directory.
    environment.preflight = [] {
        const auto elevated = require_elevated_administrator();
        if (!elevated.ok()) return elevated;
        return require_phdskmnt_service_available();
    };
    environment.classify_bundle = [](const std::filesystem::path& path) {
        return classify_bundle(path);
    };
    // M3 holds the authoritative BundleWriterLock.  Generation-zero creation
    // and M3AimRuntime::start each acquire it around their own mutation/open
    // window; a second adapter-level lock would self-deadlock those operations.
    environment.acquire_bundle_lock = [](const std::filesystem::path&) {
        return Result<void>::success();
    };
    environment.release_bundle_lock = [] {
        return Result<void>::success();
    };
    return environment;
}

Result<DiskRuntimeOperations> make_runtime_operations(
    RuntimeAdapterDependencies& dependencies) {
    const auto& environment = dependencies.environment;
    const auto incomplete = [](const wchar_t* operation) {
        return Result<DiskRuntimeOperations>::failure(adapter_error(
            ErrorDomain::internal, 1, L"make_runtime_operations",
            operation));
    };
    if (!environment.preflight)
        return incomplete(L"preflight gate is not configured");
    if (!environment.classify_bundle)
        return incomplete(L"classify_bundle gate is not configured");
    if (!environment.acquire_bundle_lock)
        return incomplete(L"acquire_bundle_lock gate is not configured");
    if (!environment.release_bundle_lock)
        return incomplete(L"release_bundle_lock gate is not configured");

    auto& volumes = dependencies.volumes;
    auto& m3 = dependencies.m3;

    DiskRuntimeOperations operations{};
    operations.preflight = [environment] { return environment.preflight(); };
    operations.classify_bundle =
        [environment](const std::filesystem::path& path) {
            return environment.classify_bundle(path);
        };
    operations.acquire_bundle_lock =
        [environment](const std::filesystem::path& path) {
            return environment.acquire_bundle_lock(path);
        };
    operations.release_bundle_lock = [environment] {
        return environment.release_bundle_lock();
    };
    operations.create_generation_zero_image = [&m3](const std::uint64_t bytes) {
        return m3.create_generation_zero(bytes);
    };
    operations.recover_complete_image = [&m3](const std::uint64_t bytes) {
        return m3.recover(bytes);
    };
    operations.start_aim = [&m3] { return m3.start(); };
    operations.stop_aim = [&m3] { return m3.stop(); };
    operations.provision_new_volume =
        [&volumes](const std::uint64_t bytes, const wchar_t letter) {
            return volumes.provision(bytes, letter);
        };
    operations.find_recovered_volume = [&volumes](const wchar_t letter) {
        return volumes.find(letter);
    };
    operations.verify_volume = [&volumes](const ProductVolume& volume,
                                          const std::uint64_t bytes) {
        return volumes.verify(volume, bytes);
    };
    operations.flush_volume = [&volumes](const ProductVolume& volume) {
        return volumes.flush(volume);
    };
    operations.assign_drive_letter = [&volumes](const ProductVolume& volume,
                                                const wchar_t letter) {
        return volumes.assign(volume, letter);
    };
    operations.dismount_volume = [&volumes](const ProductVolume& volume) {
        return volumes.dismount(volume);
    };
    operations.remove_drive_letter = [&volumes](const ProductVolume& volume) {
        return volumes.remove_letter(volume);
    };
    operations.checkpoint = [&m3] { return m3.checkpoint(); };
    operations.maintenance_stats = [&m3] { return m3.maintenance_stats(); };
    operations.prepare_checkpoint =
        [&m3](const DiskCheckpointPrepareRequest& request) {
            return m3.prepare_checkpoint(request);
        };
    operations.commit_checkpoint =
        [&m3](const DiskCheckpointCommitRequest& request) {
            return m3.commit_checkpoint(request);
        };
    operations.abort_checkpoint = [&m3](const std::uint64_t native_operation_id) {
        return m3.abort_checkpoint(native_operation_id);
    };
    return Result<DiskRuntimeOperations>::success(std::move(operations));
}

struct ProductionRuntimeAdapter::Impl final {
    explicit Impl(const Config& config)
        : persistence({config.bundle, config.virtual_bytes, "disk.rdm",
                       config.image_uuid}),
          owner_token(config.owner_token),
          ram_limit_bytes(config.ram_limit_bytes) {}

    M4PersistenceAdapter persistence;
    Win32VolumeAdapter volume_adapter;
    std::unique_ptr<m3::M3AimRuntime> aim_runtime;
    std::wstring owner_token;
    std::uint64_t ram_limit_bytes{};
    OperationContext context{};
};

ProductionRuntimeAdapter::ProductionRuntimeAdapter(Config config)
    : config_(std::move(config)), impl_(std::make_unique<Impl>(config_)) {}

ProductionRuntimeAdapter::~ProductionRuntimeAdapter() noexcept = default;

Result<std::uint32_t> ProductionRuntimeAdapter::aim_disk_number(
    const wchar_t* operation) const {
    if (!impl_->aim_runtime) {
        return Result<std::uint32_t>::failure(adapter_error(
            ErrorDomain::internal, 1, operation, L"M3 runtime is unavailable"));
    }
    return Result<std::uint32_t>::success(
        impl_->aim_runtime->info().disk.physical_drive_number);
}

Result<void> ProductionRuntimeAdapter::create_generation_zero(
    const std::uint64_t bytes) {
    if (bytes != config_.virtual_bytes) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::usage, 1, L"create_generation_zero_image",
            L"requested image size changed during startup"));
    }
    const auto directory = ensure_bundle_directory(config_.bundle);
    if (!directory.ok()) return directory;
    const auto created = impl_->persistence.create_generation_zero(impl_->context);
    if (!created.ok()) return Result<void>::failure(created.error());
    return Result<void>::success();
}

Result<void> ProductionRuntimeAdapter::recover(const std::uint64_t bytes) {
    if (bytes != config_.virtual_bytes) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::usage, 1, L"recover_complete_image",
            L"requested image size changed during startup"));
    }
    const auto recovered = impl_->persistence.recover_complete(impl_->context);
    if (!recovered.ok()) return Result<void>::failure(recovered.error());
    if (recovered.value().checkpoint_generation == 0) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::persistence, 1, L"recover_complete_image",
            L"generation-zero bundle has no durable Windows volume checkpoint"));
    }
    return Result<void>::success();
}

Result<void> ProductionRuntimeAdapter::start() {
    m3::M3AimProviderFactoryConfig factory{};
    factory.context = impl_->context;
    factory.provider.bundle_root = config_.bundle;
    factory.provider.manifest_name = "disk.rdm";
    factory.provider.recovery.limits =
        persistence::production_persistence_limits(config_.virtual_bytes);
    factory.provider.store = store_options_for(impl_->ram_limit_bytes);
    factory.provider.request_timeout = std::chrono::seconds(30);

    auto created = m3::M3AimRuntime::create(std::move(factory));
    if (!created.ok()) return Result<void>::failure(created.error());
    impl_->aim_runtime = created.move_value();
    const auto started = impl_->aim_runtime->start();
    if (!started.ok()) {
        (void)impl_->aim_runtime->stop();
        impl_->aim_runtime.reset();
        return started;
    }
    if (impl_->aim_runtime->info().virtual_bytes != config_.virtual_bytes) {
        (void)impl_->aim_runtime->stop();
        impl_->aim_runtime.reset();
        return Result<void>::failure(adapter_error(
            ErrorDomain::persistence, 1, L"start_aim",
            L"M3 runtime geometry does not match requested size"));
    }
    return Result<void>::success();
}

Result<void> ProductionRuntimeAdapter::stop() {
    if (!impl_->aim_runtime) return Result<void>::success();
    const auto stopped = impl_->aim_runtime->stop();
    if (stopped.ok()) impl_->aim_runtime.reset();
    return stopped;
}

Result<ProductVolume> ProductionRuntimeAdapter::provision(const std::uint64_t,
                                                          const wchar_t letter) {
    const auto disk = aim_disk_number(L"provision_new_volume");
    if (!disk.ok()) return Result<ProductVolume>::failure(disk.error());
    return impl_->volume_adapter.provision_and_adopt_volume(
        disk.value(), letter, impl_->owner_token, 120000);
}

Result<ProductVolume> ProductionRuntimeAdapter::find(const wchar_t letter) {
    const auto disk = aim_disk_number(L"find_recovered_volume");
    if (!disk.ok()) return Result<ProductVolume>::failure(disk.error());
    return impl_->volume_adapter.find_recovered_volume(disk.value(), letter,
                                                       impl_->owner_token);
}

Result<void> ProductionRuntimeAdapter::verify(const ProductVolume& volume,
                                              const std::uint64_t expected_bytes) {
    if (!impl_->aim_runtime || expected_bytes != config_.virtual_bytes) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::internal, 1, L"verify_volume",
            L"M3 runtime is unavailable or geometry changed"));
    }
    const auto observed = impl_->volume_adapter.observe(volume);
    if (!observed.ok()) return Result<void>::failure(observed.error());
    const auto& actual = observed.value().volume;
    auto normalized_guid = [](std::wstring value) {
        while (!value.empty() && value.back() == L'\\') value.pop_back();
        return value;
    };
    const auto actual_guid = normalized_guid(actual.volume_guid);
    const auto expected_guid = normalized_guid(volume.volume_guid);
    const auto extent_bytes =
        actual.extents.empty() ? 0ULL : actual.extents.front().length_bytes;
    constexpr std::uint64_t kPartitionOverheadAllowance = 2ULL * 1024ULL * 1024ULL;
    if (!actual.operation_owned ||
        actual.extents.size() != 1 ||
        extent_bytes == 0 || extent_bytes > expected_bytes ||
        expected_bytes - extent_bytes > kPartitionOverheadAllowance ||
        actual.extents.front().disk_number !=
            impl_->aim_runtime->info().disk.physical_drive_number ||
        _wcsicmp(actual_guid.c_str(), expected_guid.c_str()) != 0 ||
        actual.extents != volume.extents) {
        std::wcerr << L"Diagnostic verify_volume: volume_guid="
                   << actual.volume_guid << L" aim_disk="
                   << impl_->aim_runtime->info().disk.physical_drive_number
                   << L" actual_disk="
                   << (actual.extents.empty() ? 0xFFFFFFFFu :
                       actual.extents.front().disk_number)
                   << L" expected_disk="
                   << (volume.extents.empty() ? 0xFFFFFFFFu :
                       volume.extents.front().disk_number) << L"\n";
        return Result<void>::failure(adapter_error(
            ErrorDomain::evidence, 1, L"verify_volume",
            L"Windows volume identity does not match the AIM disk"));
    }
    return Result<void>::success();
}

Result<void> ProductionRuntimeAdapter::flush(const ProductVolume& volume) {
    return impl_->volume_adapter.flush(volume);
}

Result<void> ProductionRuntimeAdapter::assign(const ProductVolume& volume,
                                              const wchar_t letter) {
    return impl_->volume_adapter.assign_drive_letter(volume, letter);
}

Result<void> ProductionRuntimeAdapter::remove_letter(const ProductVolume& volume) {
    return impl_->volume_adapter.remove_drive_letter(volume);
}

Result<void> ProductionRuntimeAdapter::dismount(const ProductVolume& volume) {
    auto locked = impl_->volume_adapter.lock(volume, 30000);
    if (!locked.ok()) return Result<void>::failure(locked.error());
    if (locked.value().busy || !locked.value().locked.has_value()) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::cleanup, 1, L"dismount_volume",
            L"volume is busy; close applications using the RAM disk and retry"));
    }
    return locked.value().locked->dismount();
}

Result<DiskCheckpointResult> ProductionRuntimeAdapter::checkpoint() {
    if (!impl_->aim_runtime) {
        return Result<DiskCheckpointResult>::failure(adapter_error(
            ErrorDomain::internal, 1, L"checkpoint", L"M3 runtime is unavailable"));
    }
    const auto outcome = impl_->aim_runtime->checkpoint(
        impl_->context, persistence::VolumeBarrierKind::WindowsVolumeFlushed);
    return Result<DiskCheckpointResult>::success(
        {map_disposition(outcome.disposition), outcome.generation});
}

DiskMaintenanceStats ProductionRuntimeAdapter::maintenance_stats() const {
    if (!impl_->aim_runtime) return {};
    const auto stats = impl_->aim_runtime->storage_stats();
    // `reserved_bytes` is RAM committed by clean as well as dirty blocks;
    // checkpoint scheduling is driven only by mutations that have not yet
    // reached the M3 journal.
    constexpr std::uint64_t kBlockBytes = 2ULL * 1024ULL * 1024ULL;
    return {(stats.resident_dirty_blocks + stats.zero_dirty_blocks) * kBlockBytes,
            0, stats.backing_generation};
}

Result<DiskCheckpointPrepared> ProductionRuntimeAdapter::prepare_checkpoint(
    const DiskCheckpointPrepareRequest& request) {
    if (!impl_->aim_runtime) {
        return Result<DiskCheckpointPrepared>::failure(adapter_error(
            ErrorDomain::internal, 1, L"prepare_checkpoint",
            L"M3 runtime is unavailable"));
    }
    m3::M3CheckpointPrepareRequest m3_request{};
    m3_request.context = impl_->context;
    m3_request.barrier = persistence::VolumeBarrierKind::WindowsVolumeFlushed;
    m3_request.native_operation_id = request.native_operation_id;

    auto prepared = impl_->aim_runtime->prepare_checkpoint(m3_request);
    if (!prepared.ok()) {
        return Result<DiskCheckpointPrepared>::failure(prepared.error());
    }
    const auto& value = prepared.value();
    return Result<DiskCheckpointPrepared>::success(
        {value.prior_generation, value.candidate_generation,
         value.manifest_slot, value.candidate_bytes_digest});
}

Result<DiskCheckpointResult> ProductionRuntimeAdapter::commit_checkpoint(
    const DiskCheckpointCommitRequest& request) {
    if (!impl_->aim_runtime) {
        return Result<DiskCheckpointResult>::failure(adapter_error(
            ErrorDomain::internal, 1, L"commit_checkpoint",
            L"M3 runtime is unavailable"));
    }
    m3::M3CheckpointCommitRequest m3_request{};
    m3_request.context = impl_->context;
    m3_request.native_operation_id = request.native_operation_id;
    m3_request.manifest_slot = request.manifest_slot;
    m3_request.candidate_bytes_digest = request.candidate_bytes_digest;

    const auto outcome = impl_->aim_runtime->commit_checkpoint(m3_request);
    // The provider reports not_committed and commit_indeterminate as
    // dispositions, not as errors.  Only a real error is a failure here; the
    // caller must still never infer commitment from a successful call alone.
    if (outcome.error.has_value()) {
        return Result<DiskCheckpointResult>::failure(*outcome.error);
    }
    const auto disposition = map_disposition(outcome.disposition);
    DiskCheckpointResult result{};
    result.disposition = disposition;
    result.generation = outcome.generation;
    // A commit that did not cleanly land has to carry the failure that decided
    // it; the worker relays it and ServiceCore validates its own evidence
    // against it, so dropping it here would leave the service with a disposition
    // it cannot justify.
    if (!checkpoint_landed(disposition) && outcome.error_context.has_value()) {
        result.error_context =
            map_error_context(*outcome.error_context, disposition);
    }
    return Result<DiskCheckpointResult>::success(std::move(result));
}

Result<void> ProductionRuntimeAdapter::abort_checkpoint(
    const std::uint64_t native_operation_id) {
    if (!impl_->aim_runtime) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::internal, 1, L"abort_checkpoint",
            L"M3 runtime is unavailable"));
    }
    return impl_->aim_runtime->abort_checkpoint(native_operation_id);
}

} // namespace myrd
