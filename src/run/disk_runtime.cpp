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
    std::lock_guard<std::mutex> guard(mutex_);
    if (started_) return Result<void>::success();

    auto fail = [this](Result<void> result) -> Result<void> {
        if (result.ok()) return result;
        (void)shutdown_clean_locked();
        return result;
    };

    if (!operations_.preflight) return fail(missing_void(L"preflight"));
    auto result = operations_.preflight();
    if (!result.ok()) return fail(result);

    if (!operations_.classify_bundle) return fail(missing_void(L"classify_bundle"));
    auto classified = operations_.classify_bundle(options_.bundle);
    if (!classified.ok()) {
        (void)shutdown_clean_locked();
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
        (void)shutdown_clean_locked();
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
            (void)shutdown_clean_locked();
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
            (void)shutdown_clean_locked();
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
            (void)shutdown_clean_locked();
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
    std::lock_guard<std::mutex> guard(mutex_);
    if (!operations_.maintenance_stats) return {};
    return operations_.maintenance_stats();
}

std::optional<ProductVolume> DiskRuntime::adopted_volume() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return volume_;
}

Result<void> DiskRuntime::flush_volume() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_ || !volume_ || !operations_.flush_volume)
        return missing_void(L"flush_volume");
    return operations_.flush_volume(*volume_);
}

Result<DiskCheckpointResult> DiskRuntime::checkpoint() {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_ || !operations_.checkpoint)
        return missing<DiskCheckpointResult>(L"checkpoint");
    // The M4 one-shot path and the M5 two-phase path share one candidate slot.
    // Letting the one-shot call run while a candidate is prepared would publish
    // a generation the prepared transaction never observed.
    if (pending_checkpoint_)
        return Result<DiskCheckpointResult>::failure(runtime_error(
            L"checkpoint",
            L"a two-phase checkpoint is prepared; commit or abort it first"));
    return operations_.checkpoint();
}

Result<DiskCheckpointPrepared> DiskRuntime::prepare_checkpoint(
    const DiskCheckpointPrepareRequest& request) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_ || !operations_.prepare_checkpoint)
        return missing<DiskCheckpointPrepared>(L"prepare_checkpoint");
    if (pending_checkpoint_)
        return Result<DiskCheckpointPrepared>::failure(runtime_error(
            L"prepare_checkpoint",
            L"a checkpoint candidate is already prepared"));
    // Zero is the "no operation" encoding and cannot identify a prepare, so it
    // is refused before any state changes (design spec 3.3).
    if (request.native_operation_id == 0)
        return Result<DiskCheckpointPrepared>::failure(runtime_error(
            L"prepare_checkpoint",
            L"zero native operation id cannot identify an M5 checkpoint"));

    auto prepared = operations_.prepare_checkpoint(request);
    if (!prepared.ok()) return prepared;
    pending_native_operation_id_ = request.native_operation_id;
    pending_checkpoint_ = prepared.value();
    return prepared;
}

Result<DiskCheckpointResult> DiskRuntime::commit_checkpoint(
    const DiskCheckpointCommitRequest& request) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_ || !operations_.commit_checkpoint)
        return missing<DiskCheckpointResult>(L"commit_checkpoint");
    if (!pending_checkpoint_)
        return Result<DiskCheckpointResult>::failure(runtime_error(
            L"commit_checkpoint", L"no checkpoint candidate is prepared"));
    if (request.native_operation_id != pending_native_operation_id_ ||
        request.manifest_slot != pending_checkpoint_->manifest_slot ||
        request.candidate_bytes_digest !=
            pending_checkpoint_->candidate_bytes_digest) {
        // The candidate stays prepared: a caller that repeats the wrong
        // identity must not be able to publish it, and must still be able to
        // retry with the recorded identity or abort.
        return Result<DiskCheckpointResult>::failure(runtime_error(
            L"commit_checkpoint",
            L"commit does not match the prepared candidate"));
    }

    auto committed = operations_.commit_checkpoint(request);
    // Once M3 has been asked to publish, the transaction belongs to M3.  An
    // indeterminate outcome is M3's to reconcile, so this runtime must never
    // attempt a later abort of it (design spec 3.5).
    pending_checkpoint_.reset();
    return committed;
}

Result<void> DiskRuntime::abort_checkpoint(
    const std::uint64_t native_operation_id) {
    std::lock_guard<std::mutex> guard(mutex_);
    if (!started_ || !operations_.abort_checkpoint)
        return missing_void(L"abort_checkpoint");
    if (!pending_checkpoint_)
        return Result<void>::failure(runtime_error(
            L"abort_checkpoint", L"no checkpoint candidate is prepared"));
    if (native_operation_id != pending_native_operation_id_)
        return Result<void>::failure(runtime_error(
            L"abort_checkpoint",
            L"abort does not match the prepared candidate"));
    return abort_pending_checkpoint_locked();
}

bool DiskRuntime::checkpoint_prepared() const {
    std::lock_guard<std::mutex> guard(mutex_);
    return pending_checkpoint_.has_value();
}

Result<void> DiskRuntime::abort_pending_checkpoint_locked() {
    if (!pending_checkpoint_) return Result<void>::success();
    if (!operations_.abort_checkpoint)
        return missing_void(L"abort_checkpoint");
    auto aborted = operations_.abort_checkpoint(pending_native_operation_id_);
    if (aborted.ok()) {
        pending_checkpoint_.reset();
        pending_native_operation_id_ = 0;
    }
    return aborted;
}

Result<void> DiskRuntime::shutdown_clean() {
    std::lock_guard<std::mutex> guard(mutex_);
    return shutdown_clean_locked();
}

Result<void> DiskRuntime::shutdown_clean_locked() {
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

    // An unpublished prepare must be released before M3 is stopped.  Stopping
    // M3 while a transaction is pending would let the provider resume and
    // publish a candidate no caller authorised (design spec 3.5, rule 1).
    if (pending_checkpoint_) {
        record(abort_pending_checkpoint_locked());
        if (pending_checkpoint_) {
            // The abort is unproven, so the transaction is still live: retain
            // everything, including AIM and the writer lock, for a later retry.
            started_ = true;
            return first_error;
        }
    }

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
