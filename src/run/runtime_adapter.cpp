#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include "src/common/diagnostic_trace.h"
#include "src/run/runtime_adapter.h"

#include "src/m3/m3_windows_acceptance.h"
#include "src/persistence/production_policy.h"
#include "src/run/driver_preflight.h"
#include "src/run/m4_persistence_adapter.h"
#include "src/storage/block_constants.h"

#include <windows.h>

#include <chrono>
#include <iostream>
#include <thread>
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
    operations.flush_volume = [&volumes](const ProductVolume& volume,
                                         std::optional<LockedVolume>& held) {
        return volumes.flush(volume, held);
    };
    operations.assign_drive_letter = [&volumes](const ProductVolume& volume,
                                                const wchar_t letter) {
        return volumes.assign(volume, letter);
    };
    operations.lock_volume = [&volumes](const ProductVolume& volume,
                                        const std::uint64_t timeout) {
        return volumes.lock(volume, static_cast<std::uint32_t>(timeout));
    };
    operations.dismount_volume = [&volumes](const ProductVolume& volume,
                                            std::optional<LockedVolume>& held) {
        return volumes.dismount(volume, held);
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
    // **Refuses rather than reporting a zero, and the only caller is the create path.**
    //
    // An attach-only runtime never locates the disk, so it has no number to give.  Returning the
    // default-constructed value would announce "physical drive 0" -- wrong, and indistinguishable from
    // the truth on a single-disk machine, which is the worst property for a number that decides where a
    // volume gets provisioned.
    //
    // Reaching this with no disk means a create was wired up as attach-only, which is a composition bug
    // and should say so rather than provision on the wrong disk.
    if (!impl_->aim_runtime->info().disk.has_value()) {
        return Result<std::uint32_t>::failure(adapter_error(
            ErrorDomain::internal, 2, operation,
            L"the runtime did not locate a physical drive, so it cannot name the disk to act on"));
    }
    return Result<std::uint32_t>::success(
        impl_->aim_runtime->info().disk->physical_drive_number);
}

Result<void> ProductionRuntimeAdapter::create_generation_zero(
    const std::uint64_t bytes) {
    // **Trace that generation zero actually ran, because "did it run?" is now the open question.**
    //
    // The seventh create in a run located a disk of its own that was not empty, while the first six located
    // their own disks and found zeros.  Every external explanation has been excluded -- one device in the
    // driver, one disk of our size, no named shared memory under M2 -- so the difference has to be in what
    // that create did rather than in what the machine held.  The first thing to establish is therefore
    // whether the bundle was freshly created at all, and a line here answers it by its presence or absence
    // next to the create's other traces.
    diagnostic_trace("M4Adapter::create_generation_zero: entered");
    if (bytes != config_.virtual_bytes) {
        return Result<void>::failure(adapter_error(
            ErrorDomain::usage, 1, L"create_generation_zero_image",
            L"requested image size changed during startup"));
    }
    const auto directory = ensure_bundle_directory(config_.bundle);
    if (!directory.ok()) return directory;
    const auto created = impl_->persistence.create_generation_zero(impl_->context);
    if (!created.ok()) return Result<void>::failure(created.error());
    diagnostic_trace("M4Adapter::create_generation_zero: wrote generation zero");
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
    // Attach-only runtimes do not locate the physical drive: see the field's declaration for why that
    // is the fix for the eleven-second block rather than an optimization.
    factory.attach_only = config_.attach_only;

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
    // **No drive number is obtained, and that is the fix rather than an omission.**
    //
    // This used to call `aim_disk_number`, which reached the physical-drive locator, which opened every
    // candidate disk to read its SCSI address and device number.  Measured: one of those opens took 11017
    // ms while a device was retiring -- the eleven-second block that made reclaim fail.
    //
    // The number was never this call's identity check.  It only filtered which volume to adopt, and
    // `verify_volume` immediately afterwards compares the volume's GUID against the durable profile -- a
    // stronger identity than a drive number, which Windows reuses.  Passing `nullopt` keeps every other
    // filter in `select_volume_for_disk` and drops only the one that was expensive to satisfy.
    // The durable identity, when this adapter has one, is what narrows the candidates: see the field's
    // declaration for why the letter filter does not.
    //
    // **A volume whose device was just created is not enumerable at that instant.**
    //
    // Measured on the guest: 3 of 20 `-Repeat 20` restarts failed at
    // `step=find_recovered_volume detail=no volume carries the recorded identity`
    // (native `ERROR_NOT_FOUND`), and the same disk restarted cleanly in the next
    // iteration -- the enumeration ran before the freshly re-created device had
    // published its volume.  The create path already tolerates exactly this: it
    // re-enumerates inside a bounded budget rather than failing on the first look
    // (`windows_volume.cpp`, the retry around `provision_and_adopt_volume`).  The
    // restart path did not, so it turned a publication delay into a failed start.
    //
    // Only `not_found` is retried, and only for a bounded time: a genuine absence --
    // a volume that really is not this disk's -- still fails, still names
    // `not_found`, and just arrives a few seconds later.
    const auto attempt = [this, letter]() {
        return impl_->volume_adapter.find_recovered_volume(
            std::nullopt, letter, impl_->owner_token,
            config_.expected_volume_guid);
    };
    auto found = attempt();
    if (found.ok()
        || found.error().code
            != static_cast<std::uint32_t>(VolumeError::not_found)
        // Only the recorded identity can be "not published yet".  Without one there is
        // nothing to wait for: the caller is matching whatever is on the machine, so a
        // missing match is the answer rather than a delay -- which is the reclaim case,
        // where the residue may genuinely be gone.
        || !config_.expected_volume_guid.has_value()
        || config_.expected_volume_guid->empty()) {
        return found;
    }
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(5000);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        auto retried = attempt();
        if (retried.ok()) return retried;
        if (retried.error().code
            != static_cast<std::uint32_t>(VolumeError::not_found)) {
            return retried;
        }
    }
    return found;
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
    // **Against the durable record when there is one, against the adopted volume otherwise.**
    //
    // The second case is weaker and says so: the adopted volume's GUID came off the same volume during
    // enumeration, so comparing against it only proves the volume was not swapped between find and
    // verify.  The first case is the real identity check -- it asks whether this is the volume the
    // profile recorded, which is the question that matters after a power loss.
    const auto expected_guid = normalized_guid(
        config_.expected_volume_guid.has_value() && !config_.expected_volume_guid->empty()
            ? *config_.expected_volume_guid
            : volume.volume_guid);
    const auto extent_bytes =
        actual.extents.empty() ? 0ULL : actual.extents.front().length_bytes;
    constexpr std::uint64_t kPartitionOverheadAllowance = 2ULL * 1024ULL * 1024ULL;
    // **The physical-disk clause is gone, and the GUID clause is what makes that sound.**
    //
    // It compared the volume's disk number against the AIM device's, which required knowing which
    // physical drive the device had become -- the value whose only source is opening every candidate
    // disk, measured blocking for 11 seconds while a device retires.
    //
    // What it was standing in for is already here twice: `actual_guid` must equal the GUID carried by
    // the `ProductVolume` this runtime adopted, and that GUID came from the durable profile.  A GUID is
    // a stronger identifier than a drive number, which Windows reuses after a disk is removed.  The
    // extent and ownership clauses are unchanged, so a volume that is not ours still fails -- it simply
    // fails on the identity that cannot collide instead of the one that can.
    // **Ownership only means something for a volume this operation created.**
    //
    // `enumerate_native_volumes` sets `operation_owned = false` on every volume it
    // observes (`windows_volume.cpp:490`); only the provisioning path sets it true.
    // Requiring it here therefore made every *recovery* fail, no matter how exact
    // the identity was -- measured as `owned=0` with the volume GUID, disk number
    // and extents all equal.  An attach-only runtime adopted a volume it cannot
    // own by definition; the durable GUID is the identity that matters there.
    const bool ownership_required = !config_.attach_only;
    if ((ownership_required && !actual.operation_owned) ||
        actual.extents.size() != 1 ||
        extent_bytes == 0 || extent_bytes > expected_bytes ||
        expected_bytes - extent_bytes > kPartitionOverheadAllowance ||
        _wcsicmp(actual_guid.c_str(), expected_guid.c_str()) != 0 ||
        actual.extents != volume.extents) {
        // **Which clause refused, not just that one did.**  The guid and disk
        // number matched when this last fired, so the reader was left with five
        // possible reasons and no way to tell them apart.
        std::wcerr << L"Diagnostic verify_volume: owned=" << actual.operation_owned
                   << L" extents=" << actual.extents.size()
                   << L" extents_equal=" << (actual.extents == volume.extents)
                   << L" extent_bytes=" << extent_bytes
                   << L" expected_bytes=" << expected_bytes
                   << L" overhead_allowance=" << kPartitionOverheadAllowance
                   << L" volume_guid=" << actual.volume_guid
                   << L" expected_guid=" << volume.volume_guid
                   << L" actual_disk="
                   << (actual.extents.empty() ? 0xFFFFFFFFu :
                       actual.extents.front().disk_number)
                   << L" expected_disk="
                   << (volume.extents.empty() ? 0xFFFFFFFFu :
                       volume.extents.front().disk_number) << L"\n";
        return Result<void>::failure(adapter_error(
            ErrorDomain::evidence, 1, L"verify_volume",
            L"Windows volume identity does not match the adopted volume"));
    }
    return Result<void>::success();
}

Result<void> ProductionRuntimeAdapter::flush(const ProductVolume& volume,
                                            std::optional<LockedVolume>& held) {
    if (held.has_value()) {
        // Through the retained freeze.  This is not an optimization: the freeze
        // opened the volume with no sharing mode, so `Win32VolumeAdapter::flush`
        // -- which opens its own handle -- cannot open it at all while the
        // runtime holds the lock.  That is what a frozen volume *means*, and it
        // is why M4's shutdown order is `lock -> verify -> flush -> checkpoint`:
        // every step after the lock runs through the handle that holds it.
        return held->flush();
    }
    return impl_->volume_adapter.flush(volume);
}

Result<void> ProductionRuntimeAdapter::assign(const ProductVolume& volume,
                                              const wchar_t letter) {
    return impl_->volume_adapter.assign_drive_letter(volume, letter);
}

Result<void> ProductionRuntimeAdapter::remove_letter(const ProductVolume& volume) {
    return impl_->volume_adapter.remove_drive_letter(volume);
}

Result<VolumeLockAttempt> ProductionRuntimeAdapter::lock(const ProductVolume& volume,
                                                         const std::uint32_t timeout_ms) {
    return impl_->volume_adapter.lock(volume, timeout_ms);
}

Result<void> ProductionRuntimeAdapter::dismount(const ProductVolume& volume,
                                                std::optional<LockedVolume>& held) {
    // **Which branch runs is traced, because the two fail for opposite reasons and the reading
    // could not tell them apart.**  A dismantle that blocks through a lock this runtime already
    // holds is one problem; one that blocks trying to *acquire* a lock is another, and the second
    // has a budget of its own that a reading must be able to check.
    myrd::diagnostic_trace(held.has_value()
                               ? "dismount: through the retained lock"
                               : "dismount: re-acquiring the lock");
    if (held.has_value()) {
        // The runtime already froze this volume, and that lock is the one to
        // dismantle through.  Re-acquiring would open the volume for exclusive
        // access a second time while this process holds it, which Win32 refuses
        // as busy -- so a `remove_mount` that re-locked here failed against the
        // very freeze the stop had correctly taken.
        //
        // Dismantling through the retained lock is also the stronger order: the
        // FSCTL_DISMOUNT_VOLUME is issued on the handle that has held the volume
        // frozen across the flush, the commit and the mount removal, so nothing
        // can slip a write in between the commit and the teardown.
        return held->dismount();
    }
    auto locked = impl_->volume_adapter.lock(volume, 30000);
    if (!locked.ok()) return Result<void>::failure(locked.error());
    if (locked.value().busy || !locked.value().locked.has_value()) {
        // The native code travels with the refusal rather than being dropped.
        // "The volume is busy" names the decision but not the cause, and the
        // cause is what a caller has to act on: a sharing violation is another
        // process using the disk, `ERROR_NOT_READY` is a volume still settling,
        // and an access denial is a permissions problem.  Reporting `native=0`
        // for all three -- which this did -- makes an intermittent teardown
        // failure indistinguishable from a deterministic one.
        return Result<void>::failure(adapter_error(
            ErrorDomain::cleanup, 1, L"dismount_volume",
            L"volume is busy; close applications using the RAM disk and retry",
            locked.value().native_code));
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

std::uint64_t dirty_bytes_from_block_counts(
    const std::uint64_t resident_dirty_blocks,
    const std::uint64_t zero_dirty_blocks) noexcept {
    // Both counters are in the store's blocks, and the store's block is
    // `kBlockSizeBytes`.  Multiplying by a second, larger constant produced a
    // number that was 32x the truth and was compared against a byte threshold.
    return (resident_dirty_blocks + zero_dirty_blocks) * kBlockSizeBytes;
}

DiskMaintenanceStats ProductionRuntimeAdapter::maintenance_stats() const {
    if (!impl_->aim_runtime) return {};
    const auto stats = impl_->aim_runtime->storage_stats();
    // `reserved_bytes` is RAM committed by clean as well as dirty blocks;
    // checkpoint scheduling is driven only by mutations that have not yet
    // reached the M3 journal.
    return {dirty_bytes_from_block_counts(stats.resident_dirty_blocks,
                                          stats.zero_dirty_blocks),
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
