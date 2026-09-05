#include "src/host/volume_flush.h"
#include "src/common/unique_handle.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <exception>
#include <string>
#include <utility>

namespace myrd::host {
namespace {

Error flush_error(
    VolumeFlushError code,
    const wchar_t* operation,
    const wchar_t* message,
    ErrorDomain domain = ErrorDomain::internal,
    std::uint32_t native_code = 0) {
    return {domain,
            static_cast<std::uint32_t>(code),
            operation,
            message,
            native_code};
}

template <typename Function>
auto invoke_platform(
    Function&& function,
    const wchar_t* operation) -> decltype(function()) {
    using ResultType = decltype(function());
    try {
        return function();
    } catch (...) {
        return ResultType::failure(flush_error(
            VolumeFlushError::platform_exception,
            operation,
            L"The volume platform threw an exception"));
    }
}

bool begin_step(
    const OperationContext& context,
    VolumeFlushOutcome& outcome) {
    auto live = check_operation_context(context);
    if (live.ok()) return true;
    outcome.error = live.error();
    return false;
}

bool binding_matches_identity(
    const WorkerBinding& binding,
    const VerifiedVolumeIdentity& identity) noexcept {
    const auto& owner = identity.owner();
    return binding.profile_id == owner.profile_uuid
        && binding.image_id == owner.image_uuid
        && binding.profile_revision == owner.profile_revision
        && binding.session_generation == owner.session_generation;
}

struct OpenedAndVerified final {
    std::unique_ptr<IVolumeFlushHandle> handle;
    VerifiedVolumeIdentity identity;
};

Result<OpenedAndVerified> open_and_reverify(
    IVolumeFlushPlatform& platform,
    const VerifiedVolumeIdentity& verified,
    const OperationContext& context,
    VolumeFlushOutcome& outcome) {
    auto opened = invoke_platform(
        [&] { return platform.open_exact(verified, context); },
        L"volume_flush.open_exact");
    if (!opened.ok()) {
        return Result<OpenedAndVerified>::failure(opened.error());
    }
    auto handle = opened.move_value();
    if (!handle) {
        return Result<OpenedAndVerified>::failure(flush_error(
            VolumeFlushError::platform_exception,
            L"volume_flush.open_exact",
            L"The volume platform returned a null handle"));
    }
    outcome.opened = true;
    auto observed = invoke_platform(
        [&] { return handle->observe(context); },
        L"volume_flush.observe");
    if (!observed.ok()) {
        return Result<OpenedAndVerified>::failure(observed.error());
    }
    auto reverified = reverify_volume_identity(
        verified, observed.value());
    if (!reverified.ok()) {
        return Result<OpenedAndVerified>::failure(reverified.error());
    }
    outcome.identity_reverified = true;
    return Result<OpenedAndVerified>::success({
        std::move(handle), reverified.move_value()});
}

void unlock_after_failure(
    IVolumeFlushHandle& handle,
    VolumeFlushOutcome& outcome) {
    outcome.unlock_attempted = true;
    auto unlocked = invoke_platform(
        [&] { return handle.unlock(OperationContext{}); },
        L"volume_flush.unlock_cleanup");
    outcome.unlocked = unlocked.ok();
    if (!unlocked.ok()) outcome.cleanup_error = unlocked.error();
}

VolumeFlushOutcome begin_verified_operation(
    IVolumeFlushPlatform& platform,
    const VerifiedVolumeIdentity& verified,
    const OperationContext& context,
    std::optional<OpenedAndVerified>& opened) {
    VolumeFlushOutcome outcome{};
    if (!begin_step(context, outcome)) return outcome;
    auto result = open_and_reverify(platform, verified, context, outcome);
    if (!result.ok()) {
        outcome.error = result.error();
        return outcome;
    }
    opened = result.move_value();
    return outcome;
}

Error windows_step_error(
    VolumeFlushError code,
    const wchar_t* operation,
    const wchar_t* message,
    DWORD native_code) {
    return flush_error(
        code,
        operation,
        message,
        ErrorDomain::win32,
        native_code);
}

Result<void> check_windows_context(
    const OperationContext& context) {
    return check_operation_context(context);
}

class WindowsVolumeFlushHandle final : public IVolumeFlushHandle {
public:
    WindowsVolumeFlushHandle(
        UniqueHandle volume_handle,
        UniqueHandle disk_handle,
        VolumeIdentityExpectation expected) noexcept
        : volume_handle_(std::move(volume_handle)),
          disk_handle_(std::move(disk_handle)),
          expected_(std::move(expected)) {}

    Result<VolumeIdentityObservation> observe(
        const OperationContext& context) override {
        auto live = check_windows_context(context);
        if (!live.ok()) {
            return Result<VolumeIdentityObservation>::failure(live.error());
        }
        return observe_open_windows_volume_identity(
            reinterpret_cast<std::uintptr_t>(volume_handle_.get()),
            reinterpret_cast<std::uintptr_t>(disk_handle_.get()),
            expected_);
    }

    Result<bool> is_mounted(const OperationContext& context) override {
        auto live = check_windows_context(context);
        if (!live.ok()) return Result<bool>::failure(live.error());
        DWORD returned = 0;
        if (DeviceIoControl(
                volume_handle_.get(),
                FSCTL_IS_VOLUME_MOUNTED,
                nullptr,
                0,
                nullptr,
                0,
                &returned,
                nullptr)) {
            return Result<bool>::success(true);
        }
        const DWORD error = GetLastError();
        if (error == ERROR_NOT_READY) return Result<bool>::success(false);
        return Result<bool>::failure(windows_step_error(
            VolumeFlushError::identity_changed,
            L"FSCTL_IS_VOLUME_MOUNTED",
            L"Failed to query the exact volume mount state",
            error));
    }

    Result<void> lock(const OperationContext& context) override {
        auto live = check_windows_context(context);
        if (!live.ok()) return live;
        DWORD returned = 0;
        if (DeviceIoControl(
                volume_handle_.get(),
                FSCTL_LOCK_VOLUME,
                nullptr,
                0,
                nullptr,
                0,
                &returned,
                nullptr)) {
            return Result<void>::success();
        }
        const DWORD error = GetLastError();
        const bool busy = error == ERROR_ACCESS_DENIED
            || error == ERROR_SHARING_VIOLATION
            || error == ERROR_LOCK_VIOLATION;
        return Result<void>::failure(windows_step_error(
            busy ? VolumeFlushError::volume_busy
                 : VolumeFlushError::identity_changed,
            L"FSCTL_LOCK_VOLUME",
            busy ? L"The exact volume is busy and cannot be locked"
                 : L"Failed to lock the exact volume",
            error));
    }

    Result<void> flush(const OperationContext& context) override {
        auto live = check_windows_context(context);
        if (!live.ok()) return live;
        // FlushFileBuffers is the durability boundary. Once issued, its actual
        // result wins over a later caller cancellation; no false timeout is
        // reported while Windows is still deciding the flush.
        if (FlushFileBuffers(volume_handle_.get())) {
            return Result<void>::success();
        }
        return Result<void>::failure(windows_step_error(
            VolumeFlushError::flush_failed,
            L"FlushFileBuffers(volume)",
            L"Failed to flush the exact Windows volume",
            GetLastError()));
    }

    Result<void> dismount(const OperationContext& context) override {
        auto live = check_windows_context(context);
        if (!live.ok()) return live;
        DWORD returned = 0;
        if (DeviceIoControl(
                volume_handle_.get(),
                FSCTL_DISMOUNT_VOLUME,
                nullptr,
                0,
                nullptr,
                0,
                &returned,
                nullptr)) {
            return Result<void>::success();
        }
        return Result<void>::failure(windows_step_error(
            VolumeFlushError::dismount_failed,
            L"FSCTL_DISMOUNT_VOLUME",
            L"Failed to dismount the exact locked volume",
            GetLastError()));
    }

    Result<void> unlock(const OperationContext& context) override {
        auto live = check_windows_context(context);
        if (!live.ok()) return live;
        DWORD returned = 0;
        if (DeviceIoControl(
                volume_handle_.get(),
                FSCTL_UNLOCK_VOLUME,
                nullptr,
                0,
                nullptr,
                0,
                &returned,
                nullptr)) {
            return Result<void>::success();
        }
        return Result<void>::failure(windows_step_error(
            VolumeFlushError::unlock_failed,
            L"FSCTL_UNLOCK_VOLUME",
            L"Failed to unlock the exact volume after an aborted removal",
            GetLastError()));
    }

private:
    UniqueHandle volume_handle_;
    UniqueHandle disk_handle_;
    VolumeIdentityExpectation expected_;
};

class WindowsVolumeFlushPlatform final : public IVolumeFlushPlatform {
public:
    Result<std::unique_ptr<IVolumeFlushHandle>> open_exact(
        const VerifiedVolumeIdentity& verified,
        const OperationContext& context) override {
        auto live = check_windows_context(context);
        if (!live.ok()) {
            return Result<std::unique_ptr<IVolumeFlushHandle>>::failure(
                live.error());
        }
        std::wstring volume_path = verified.observation().volume_guid_path;
        if (volume_path.empty() || volume_path.back() != L'\\') {
            return Result<std::unique_ptr<IVolumeFlushHandle>>::failure(
                flush_error(
                    VolumeFlushError::invalid_argument,
                    L"WindowsVolumeFlushPlatform::open_exact",
                    L"The verified volume GUID path is malformed"));
        }
        volume_path.pop_back();
        const auto& disk_path =
            verified.pinned_expectation().physical_disk.path;
        UniqueHandle disk_handle(CreateFileW(
            disk_path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!disk_handle) {
            return Result<std::unique_ptr<IVolumeFlushHandle>>::failure(
                windows_step_error(
                    VolumeFlushError::open_failed,
                    L"CreateFileW(verified PhysicalDrive)",
                    L"Failed to retain the exact verified PhysicalDrive",
                    GetLastError()));
        }
        UniqueHandle volume_handle(CreateFileW(
            volume_path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr));
        if (!volume_handle) {
            return Result<std::unique_ptr<IVolumeFlushHandle>>::failure(
                windows_step_error(
                    VolumeFlushError::open_failed,
                    L"CreateFileW(verified volume)",
                    L"Failed to open the exact verified volume for flush",
                    GetLastError()));
        }
        std::unique_ptr<IVolumeFlushHandle> result =
            std::make_unique<WindowsVolumeFlushHandle>(
                std::move(volume_handle),
                std::move(disk_handle),
                verified.pinned_expectation());
        return Result<std::unique_ptr<IVolumeFlushHandle>>::success(
            std::move(result));
    }
};

} // namespace

struct VolumeFlushProofFactory final {
    static VerifiedVolumeFlush create(
        VerifiedVolumeIdentity identity,
        VolumeFlushPurpose purpose,
        bool dismounted,
        bool already_unmounted) {
        return VerifiedVolumeFlush{
            std::move(identity), purpose, dismounted, already_unmounted};
    }
};

struct SuccessfulCheckpointFactory final {
    static SuccessfulCheckpoint create(
        VerifiedVolumeIdentity identity,
        std::uint64_t persistence_generation) {
        return SuccessfulCheckpoint{
            std::move(identity), persistence_generation};
    }
};

VerifiedVolumeFlush::VerifiedVolumeFlush(
    VerifiedVolumeIdentity identity,
    VolumeFlushPurpose purpose,
    bool dismounted,
    bool already_unmounted) noexcept
    : identity_(std::move(identity)),
      purpose_(purpose),
      dismounted_(dismounted),
      already_unmounted_(already_unmounted) {}

const VerifiedVolumeIdentity& VerifiedVolumeFlush::identity() const noexcept {
    return identity_;
}

VolumeFlushPurpose VerifiedVolumeFlush::purpose() const noexcept {
    return purpose_;
}

bool VerifiedVolumeFlush::dismounted() const noexcept { return dismounted_; }

bool VerifiedVolumeFlush::already_unmounted() const noexcept {
    return already_unmounted_;
}

SuccessfulCheckpoint::SuccessfulCheckpoint(
    VerifiedVolumeIdentity identity,
    std::uint64_t persistence_generation) noexcept
    : identity_(std::move(identity)),
      persistence_generation_(persistence_generation) {}

const VerifiedVolumeIdentity& SuccessfulCheckpoint::identity() const noexcept {
    return identity_;
}

std::uint64_t SuccessfulCheckpoint::persistence_generation() const noexcept {
    return persistence_generation_;
}

VolumeFlushOutcome flush_volume_for_checkpoint(
    IVolumeFlushPlatform& platform,
    const VerifiedVolumeIdentity& verified,
    const OperationContext& context) {
    std::optional<OpenedAndVerified> opened;
    auto outcome = begin_verified_operation(
        platform, verified, context, opened);
    if (!opened.has_value()) return outcome;
    if (!begin_step(context, outcome)) return outcome;
    auto flushed = invoke_platform(
        [&] { return opened->handle->flush(context); },
        L"volume_flush.checkpoint");
    if (!flushed.ok()) {
        outcome.error = flushed.error();
        return outcome;
    }
    outcome.flushed = true;
    outcome.proof = VolumeFlushProofFactory::create(
        opened->identity,
        VolumeFlushPurpose::checkpoint_barrier,
        false,
        false);
    return outcome;
}

CheckpointForRemovalOutcome checkpoint_volume_for_removal(
    IVolumeFlushPlatform& platform,
    HostCore& host,
    const HostCommand& command,
    const VerifiedVolumeIdentity& verified,
    const OperationContext& context) {
    CheckpointForRemovalOutcome outcome{};
    if (!binding_matches_identity(command.binding, verified)) {
        outcome.error = flush_error(
            VolumeFlushError::invalid_argument,
            L"checkpoint_volume_for_removal",
            L"The host command binding does not match the verified volume identity");
        return outcome;
    }

    outcome.volume_flush = flush_volume_for_checkpoint(
        platform, verified, context);
    if (!outcome.volume_flush.ok()
        || !outcome.volume_flush.proof.has_value()
        || outcome.volume_flush.proof->purpose()
            != VolumeFlushPurpose::checkpoint_barrier) {
        return outcome;
    }
    auto live = check_operation_context(context);
    if (!live.ok()) {
        outcome.error = live.error();
        return outcome;
    }

    try {
        outcome.checkpoint = host.checkpoint(
            command,
            context,
            HostVolumeBarrierKind::windows_volume_flushed);
    } catch (...) {
        outcome.error = flush_error(
            VolumeFlushError::checkpoint_failed,
            L"checkpoint_volume_for_removal",
            L"The host checkpoint threw after the Windows volume barrier");
        return outcome;
    }
    if (!outcome.checkpoint->ok()) return outcome;

    outcome.proof = SuccessfulCheckpointFactory::create(
        outcome.volume_flush.proof->identity(),
        outcome.checkpoint->generation);
    return outcome;
}

VolumeFlushOutcome prepare_volume_for_removal(
    IVolumeFlushPlatform& platform,
    const SuccessfulCheckpoint& successful_checkpoint,
    const OperationContext& context) {
    const auto& verified = successful_checkpoint.identity();
    std::optional<OpenedAndVerified> opened;
    auto outcome = begin_verified_operation(
        platform, verified, context, opened);
    if (!opened.has_value()) return outcome;
    if (!begin_step(context, outcome)) return outcome;
    auto mounted = invoke_platform(
        [&] { return opened->handle->is_mounted(context); },
        L"volume_flush.is_mounted");
    if (!mounted.ok()) {
        outcome.error = mounted.error();
        return outcome;
    }
    outcome.already_unmounted = !mounted.value();
    if (!mounted.value()) {
        if (!begin_step(context, outcome)) return outcome;
        auto flushed = invoke_platform(
            [&] { return opened->handle->flush(context); },
            L"volume_flush.removal_flush");
        if (!flushed.ok()) {
            outcome.error = flushed.error();
            return outcome;
        }
        outcome.flushed = true;
        outcome.proof = VolumeFlushProofFactory::create(
            opened->identity,
            VolumeFlushPurpose::removal_after_checkpoint,
            false,
            true);
        return outcome;
    }

    if (!begin_step(context, outcome)) return outcome;
    auto locked = invoke_platform(
        [&] { return opened->handle->lock(context); },
        L"volume_flush.lock");
    if (!locked.ok()) {
        outcome.error = locked.error();
        return outcome;
    }
    outcome.locked = true;
    if (!begin_step(context, outcome)) {
        unlock_after_failure(*opened->handle, outcome);
        return outcome;
    }
    auto flushed = invoke_platform(
        [&] { return opened->handle->flush(context); },
        L"volume_flush.removal_flush");
    if (!flushed.ok()) {
        outcome.error = flushed.error();
        unlock_after_failure(*opened->handle, outcome);
        return outcome;
    }
    outcome.flushed = true;
    if (!begin_step(context, outcome)) {
        unlock_after_failure(*opened->handle, outcome);
        return outcome;
    }
    auto dismounted = invoke_platform(
        [&] { return opened->handle->dismount(context); },
        L"volume_flush.dismount");
    if (!dismounted.ok()) {
        outcome.error = dismounted.error();
        unlock_after_failure(*opened->handle, outcome);
        return outcome;
    }
    outcome.dismounted = true;
    outcome.proof = VolumeFlushProofFactory::create(
        opened->identity,
        VolumeFlushPurpose::removal_after_checkpoint,
        true,
        false);
    return outcome;
}

std::unique_ptr<IVolumeFlushPlatform> make_windows_volume_flush_platform() {
    return std::make_unique<WindowsVolumeFlushPlatform>();
}

} // namespace myrd::host
