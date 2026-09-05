#pragma once

#include "src/common/operation_context.h"
#include "src/host/host_core.h"
#include "src/host/volume_identity.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace myrd::host {

enum class VolumeFlushError : std::uint32_t {
    invalid_argument = 0x4F01,
    open_failed,
    identity_changed,
    volume_busy,
    flush_failed,
    dismount_failed,
    unlock_failed,
    checkpoint_failed,
    platform_exception,
};

enum class VolumeFlushPurpose : std::uint8_t {
    checkpoint_barrier = 0,
    removal_after_checkpoint,
};

class VerifiedVolumeFlush final {
public:
    VerifiedVolumeFlush(const VerifiedVolumeFlush&) = default;
    VerifiedVolumeFlush& operator=(const VerifiedVolumeFlush&) = default;
    VerifiedVolumeFlush(VerifiedVolumeFlush&&) noexcept = default;
    VerifiedVolumeFlush& operator=(VerifiedVolumeFlush&&) noexcept = default;

    const VerifiedVolumeIdentity& identity() const noexcept;
    VolumeFlushPurpose purpose() const noexcept;
    bool dismounted() const noexcept;
    bool already_unmounted() const noexcept;

private:
    VerifiedVolumeFlush(
        VerifiedVolumeIdentity identity,
        VolumeFlushPurpose purpose,
        bool dismounted,
        bool already_unmounted) noexcept;

    VerifiedVolumeIdentity identity_;
    VolumeFlushPurpose purpose_{};
    bool dismounted_{};
    bool already_unmounted_{};

    friend struct VolumeFlushProofFactory;
};

class SuccessfulCheckpoint final {
public:
    SuccessfulCheckpoint(const SuccessfulCheckpoint&) = default;
    SuccessfulCheckpoint& operator=(const SuccessfulCheckpoint&) = default;
    SuccessfulCheckpoint(SuccessfulCheckpoint&&) noexcept = default;
    SuccessfulCheckpoint& operator=(SuccessfulCheckpoint&&) noexcept =
        default;

    const VerifiedVolumeIdentity& identity() const noexcept;
    std::uint64_t persistence_generation() const noexcept;

private:
    SuccessfulCheckpoint(
        VerifiedVolumeIdentity identity,
        std::uint64_t persistence_generation) noexcept;

    VerifiedVolumeIdentity identity_;
    std::uint64_t persistence_generation_{};

    friend struct SuccessfulCheckpointFactory;
};

struct VolumeFlushOutcome final {
    bool opened{};
    bool identity_reverified{};
    bool locked{};
    bool flushed{};
    bool dismounted{};
    bool already_unmounted{};
    bool unlock_attempted{};
    bool unlocked{};
    std::optional<Error> error;
    std::optional<Error> cleanup_error;
    std::optional<VerifiedVolumeFlush> proof;

    [[nodiscard]] bool ok() const noexcept {
        return proof.has_value() && !error.has_value()
            && !cleanup_error.has_value() && opened
            && identity_reverified && flushed;
    }
};

struct CheckpointForRemovalOutcome final {
    VolumeFlushOutcome volume_flush;
    std::optional<HostCheckpointOutcome> checkpoint;
    std::optional<SuccessfulCheckpoint> proof;
    std::optional<Error> error;

    [[nodiscard]] bool ok() const noexcept {
        return volume_flush.ok() && checkpoint.has_value()
            && checkpoint->ok() && proof.has_value() && !error.has_value();
    }
};

class IVolumeFlushHandle {
public:
    virtual ~IVolumeFlushHandle() = default;
    virtual Result<VolumeIdentityObservation> observe(
        const OperationContext& context) = 0;
    virtual Result<bool> is_mounted(const OperationContext& context) = 0;
    virtual Result<void> lock(const OperationContext& context) = 0;
    virtual Result<void> flush(const OperationContext& context) = 0;
    virtual Result<void> dismount(const OperationContext& context) = 0;
    virtual Result<void> unlock(const OperationContext& context) = 0;
};

class IVolumeFlushPlatform {
public:
    virtual ~IVolumeFlushPlatform() = default;
    virtual Result<std::unique_ptr<IVolumeFlushHandle>> open_exact(
        const VerifiedVolumeIdentity& verified,
        const OperationContext& context) = 0;
};

VolumeFlushOutcome flush_volume_for_checkpoint(
    IVolumeFlushPlatform& platform,
    const VerifiedVolumeIdentity& verified,
    const OperationContext& context);

CheckpointForRemovalOutcome checkpoint_volume_for_removal(
    IVolumeFlushPlatform& platform,
    HostCore& host,
    const HostCommand& command,
    const VerifiedVolumeIdentity& verified,
    const OperationContext& context);

VolumeFlushOutcome prepare_volume_for_removal(
    IVolumeFlushPlatform& platform,
    const SuccessfulCheckpoint& successful_checkpoint,
    const OperationContext& context);

std::unique_ptr<IVolumeFlushPlatform> make_windows_volume_flush_platform();

} // namespace myrd::host
