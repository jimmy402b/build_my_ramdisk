#include "src/run/disk_runtime.h"

#include <utility>

namespace myrd {
namespace {

Error runtime_error(const wchar_t* operation, const wchar_t* message) {
    return {ErrorDomain::internal, 1, operation, message, 0};
}

template <typename T>
Result<T> missing(const wchar_t* operation) {
    return Result<T>::failure(
        runtime_error(operation, L"required runtime operation is not configured"));
}

Result<void> missing_void(const wchar_t* operation) {
    return Result<void>::failure(
        runtime_error(operation, L"required runtime operation is not configured"));
}

} // namespace

DiskRuntime::DiskRuntime(DiskRuntimeMode mode, DiskRuntimeOptions options,
                         DiskRuntimeOperations operations)
    : mode_(mode), options_(std::move(options)), operations_(std::move(operations)) {}

Result<std::unique_ptr<DiskRuntime>> DiskRuntime::create(
    DiskRuntimeOptions options, DiskRuntimeOperations operations) {
    return Result<std::unique_ptr<DiskRuntime>>::success(
        std::unique_ptr<DiskRuntime>(
            new DiskRuntime(DiskRuntimeMode::create, std::move(options),
                            std::move(operations))));
}

Result<std::unique_ptr<DiskRuntime>> DiskRuntime::recover(
    DiskRuntimeOptions options, DiskRuntimeOperations operations) {
    return Result<std::unique_ptr<DiskRuntime>>::success(
        std::unique_ptr<DiskRuntime>(
            new DiskRuntime(DiskRuntimeMode::recover, std::move(options),
                            std::move(operations))));
}

DiskRuntime::~DiskRuntime() noexcept {
    // Destructors cannot report cleanup failures.  Best effort cleanup is
    // nevertheless important when a caller abandons a failed start.
    (void)shutdown_clean();
}

Result<void> DiskRuntime::start() {
    if (started_) return Result<void>::success();

    auto fail = [this](Result<void> result) -> Result<void> {
        if (result.ok()) return result;
        (void)shutdown_clean();
        return result;
    };

    if (!operations_.preflight) return fail(missing_void(L"preflight"));
    auto result = operations_.preflight();
    if (!result.ok()) return fail(result);

    if (!operations_.classify_bundle) return fail(missing_void(L"classify_bundle"));
    auto classified = operations_.classify_bundle(options_.bundle);
    if (!classified.ok()) {
        (void)shutdown_clean();
        return Result<void>::failure(classified.error());
    }
    const auto kind = classified.value();

    if (!operations_.acquire_bundle_lock)
        return fail(missing_void(L"acquire_bundle_lock"));
    result = operations_.acquire_bundle_lock(options_.bundle);
    if (!result.ok()) return fail(result);
    bundle_lock_held_ = true;

    // Reject a mode/kind mismatch only after the writer lock is acquired so
    // the failure follows the same deterministic ownership boundary as all
    // later lifecycle stages.
    if ((mode_ == DiskRuntimeMode::create && kind != DiskBundleKind::empty) ||
        (mode_ == DiskRuntimeMode::recover && kind != DiskBundleKind::manifest)) {
        auto invalid = Result<void>::failure(
            runtime_error(L"classify_bundle", L"bundle kind does not match runtime mode"));
        (void)shutdown_clean();
        return invalid;
    }

    if (mode_ == DiskRuntimeMode::create) {
        if (!operations_.create_generation_zero_image)
            return fail(missing_void(L"create_generation_zero_image"));
        result = operations_.create_generation_zero_image(options_.virtual_bytes);
    } else {
        if (!operations_.recover_complete_image)
            return fail(missing_void(L"recover_complete_image"));
        result = operations_.recover_complete_image(options_.virtual_bytes);
    }
    if (!result.ok()) return fail(result);

    if (!operations_.start_aim) return fail(missing_void(L"start_aim"));
    result = operations_.start_aim();
    if (!result.ok()) return fail(result);
    aim_started_ = true;

    if (mode_ == DiskRuntimeMode::create) {
        if (!operations_.provision_new_volume)
            return fail(missing_void(L"provision_new_volume"));
        auto provisioned = operations_.provision_new_volume(
            options_.virtual_bytes, options_.drive_letter);
        if (!provisioned.ok()) {
            (void)shutdown_clean();
            return Result<void>::failure(provisioned.error());
        }
        volume_ = provisioned.move_value();
        // Provisioning creates and mounts the volume with the requested
        // drive letter.  Record that ownership immediately so every later
        // failure path removes only the mount established by this runtime.
        drive_letter_assigned_ = true;
    } else {
        // Recovery is intentionally attach-only: the validated M3 image is
        // reopened first, then AIM is started and the pre-existing volume is
        // discovered.  Provisioning is never a recovery fallback; a missing
        // or mismatched volume must fail closed rather than format anything.
        if (!operations_.find_recovered_volume)
            return fail(missing_void(L"find_recovered_volume"));
        auto found = operations_.find_recovered_volume(options_.drive_letter);
        if (!found.ok()) {
            (void)shutdown_clean();
            return Result<void>::failure(found.error());
        }
        volume_ = found.move_value();
    }

    if (!operations_.verify_volume)
        return fail(missing_void(L"verify_volume"));
    result = operations_.verify_volume(*volume_, options_.virtual_bytes);
    if (!result.ok()) return fail(result);

    if (mode_ == DiskRuntimeMode::create) {
        if (!operations_.flush_volume) return fail(missing_void(L"flush_volume"));
        result = operations_.flush_volume(*volume_);
        if (!result.ok()) return fail(result);
        if (!operations_.checkpoint) return fail(missing_void(L"checkpoint"));
        auto committed = operations_.checkpoint();
        if (!committed.ok()) {
            (void)shutdown_clean();
            return Result<void>::failure(committed.error());
        }
        if (committed.value().disposition != DiskCheckpointDisposition::committed) {
            auto not_committed = Result<void>::failure(
                runtime_error(L"checkpoint", L"generation-zero checkpoint was not committed"));
            (void)shutdown_clean();
            return not_committed;
        }
    } else {
        if (!operations_.assign_drive_letter)
            return fail(missing_void(L"assign_drive_letter"));
        result = operations_.assign_drive_letter(*volume_, options_.drive_letter);
        if (!result.ok()) return fail(result);
        drive_letter_assigned_ = true;
    }

    started_ = true;
    return Result<void>::success();
}

DiskMaintenanceStats DiskRuntime::maintenance_stats() const {
    if (!operations_.maintenance_stats) return {};
    return operations_.maintenance_stats();
}

Result<void> DiskRuntime::flush_volume() {
    if (!started_ || !volume_ || !operations_.flush_volume)
        return missing_void(L"flush_volume");
    return operations_.flush_volume(*volume_);
}

Result<DiskCheckpointResult> DiskRuntime::checkpoint() {
    if (!started_ || !operations_.checkpoint)
        return missing<DiskCheckpointResult>(L"checkpoint");
    return operations_.checkpoint();
}

Result<void> DiskRuntime::shutdown_clean() {
    if (!started_) {
        // A failed start has already performed cleanup.  Calls before any
        // successful start are rejected and intentionally have no side effects.
        if (!bundle_lock_held_ && !aim_started_ && !volume_ &&
            !drive_letter_assigned_)
            return cleanup_attempted_ ? Result<void>::success() :
                Result<void>::failure(runtime_error(
                    L"shutdown_clean", L"runtime has not been started"));
    }
    cleanup_attempted_ = true;

    Result<void> first_error = Result<void>::success();
    auto record = [&first_error](Result<void> result) {
        if (!result.ok() && first_error.ok()) first_error = result;
    };

    if (volume_) {
        if (started_) {
            if (!operations_.flush_volume)
                record(missing_void(L"flush_volume"));
            else
                record(operations_.flush_volume(*volume_));
        }
        if (drive_letter_assigned_) {
            if (!operations_.remove_drive_letter) {
                record(missing_void(L"remove_drive_letter"));
            } else {
                auto removed = operations_.remove_drive_letter(*volume_);
                record(removed);
                // Retain ownership after a failed removal so a later explicit
                // cleanup can retry the same exact resource.  Clearing this
                // bit here would leak a mount while falsely claiming cleanup
                // done.
                if (removed.ok()) drive_letter_assigned_ = false;
            }
        }
        // A mount must remain intact until its drive letter has been removed.
        // If that child cleanup fails, retain the volume and all parents for
        // a later retry; dismounting here would strand the assigned letter.
        if (drive_letter_assigned_) {
            started_ = true;
            return first_error;
        }
        if (!volume_dismounted_) {
            if (!operations_.dismount_volume) {
                record(missing_void(L"dismount_volume"));
            } else {
                auto dismounted = operations_.dismount_volume(*volume_);
                record(dismounted);
                if (dismounted.ok()) volume_dismounted_ = true;
            }
        }
        if (volume_dismounted_) volume_.reset();
    }
    // Do not release parent resources while a child volume cleanup remains
    // incomplete.  A failed dismount is retried before AIM/lock teardown.
    if (volume_) {
        started_ = true;
        return first_error;
    }
    if (aim_started_) {
        if (!operations_.stop_aim) {
            record(missing_void(L"stop_aim"));
        } else {
            auto stopped = operations_.stop_aim();
            record(stopped);
            if (stopped.ok()) aim_started_ = false;
        }
    }
    // AIM owns the device that was backed by this bundle.  Keep the writer
    // lock until that device is definitely stopped; otherwise another writer
    // could open the bundle while the failed AIM runtime still owns it.
    if (aim_started_) {
        started_ = true;
        return first_error;
    }
    if (bundle_lock_held_) {
        if (!operations_.release_bundle_lock) {
            record(missing_void(L"release_bundle_lock"));
        } else {
            auto released = operations_.release_bundle_lock();
            record(released);
            if (released.ok()) bundle_lock_held_ = false;
        }
    }
    // A failed cleanup remains retryable.  Keep started_ true while any
    // owned resource survives, so repeated shutdown_clean() retries cleanup
    // rather than silently returning success/no-op.
    started_ = bundle_lock_held_ || aim_started_ || volume_.has_value();
    return first_error;
}

} // namespace myrd
