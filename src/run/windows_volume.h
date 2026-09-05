#pragma once

#include "src/common/result.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace myrd {

class Win32VolumeAdapter;

// These types deliberately contain only observations.  The implementation
// will translate Win32 volume/device state into them; unit tests never call
// Win32, DiskPart, or the AIM driver.
struct VolumeExtent final {
    std::uint32_t disk_number{};
    std::uint64_t starting_offset{};
    std::uint64_t length_bytes{};
    bool operator==(const VolumeExtent&) const noexcept = default;
};

struct ProductVolume final {
    std::wstring owner_token;
    std::wstring volume_guid;
    std::vector<VolumeExtent> extents;
    wchar_t drive_letter{};
    bool mounted{};
    bool operation_owned{};
    bool operator==(const ProductVolume&) const noexcept = default;
};

struct VolumeObservation final {
    ProductVolume volume;
    bool foreign_mount{};
    bool drive_letter_occupied{};
    bool lock_available{true};
    bool lock_api_error{};
    bool lock_timed_out{};
    bool flush_succeeds{true};
    bool cleanup_succeeds{true};
};

// Owns the exact volume handle returned after FSCTL_LOCK_VOLUME succeeds.
// The lock is held for the lifetime of this object; closing the handle releases
// the lock.  Keeping ownership in the result prevents a successful lock from
// being accidentally released before flush/dismount/cleanup complete.
class LockedVolume final {
public:
    LockedVolume(const LockedVolume&) = delete;
    LockedVolume& operator=(const LockedVolume&) = delete;
    LockedVolume(LockedVolume&&) noexcept;
    LockedVolume& operator=(LockedVolume&&) noexcept;
    ~LockedVolume() noexcept;

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] Result<void> flush() const;
    [[nodiscard]] Result<void> dismount() const;

private:
    friend class Win32VolumeAdapter;
#ifdef _WIN32
    explicit LockedVolume(HANDLE handle, ProductVolume identity) noexcept
        : handle_(handle), identity_(std::move(identity)) {}
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
    explicit LockedVolume(void* handle, ProductVolume identity) noexcept
        : handle_(handle), identity_(std::move(identity)) {}
    void* handle_{};
#endif
    ProductVolume identity_{};
};

enum class VolumeError : std::uint32_t {
    foreign_mount = 0x5201,
    occupied_drive_letter,
    timeout,
    flush_failed,
    busy,
    cleanup_failed,
    ownership_mismatch,
    not_found,
    platform_error,
};

// A busy volume is an expected observation, not an API failure: callers may
// keep the runtime alive and retry.  Timeout and other native failures remain
// Result failures so they cannot be mistaken for a successful lock attempt.
struct VolumeLockAttempt final {
    bool acquired{};
    bool busy{};
    std::optional<LockedVolume> locked{};

    bool operator==(const VolumeLockAttempt& other) const noexcept {
        return acquired == other.acquired && busy == other.busy;
    }
};

// Production Win32 observation/operation boundary.  The policy functions
// below remain pure and are the only functions used by unit tests; this
// adapter is used by DiskRuntime when running on Windows.
class Win32VolumeAdapter final {
public:
    // Runs the embedded, fixed provisioning script against one exact disk.
    // The child process is bounded by timeout_ms and by 64 KiB per output
    // stream; only the exact child PID may be terminated on failure.
    [[nodiscard]] Result<void> provision_new_volume(
        std::uint32_t disk_number, wchar_t drive_letter,
        std::uint32_t timeout_ms) const;
    // Provision and then adopt the one volume whose exact physical extent is
    // backed by disk_number.  The returned identity is suitable for all
    // subsequent ownership checks; no caller-supplied GUID is trusted.
    [[nodiscard]] Result<ProductVolume> provision_and_adopt_volume(
        std::uint32_t disk_number, wchar_t drive_letter,
        std::wstring owner_token, std::uint32_t timeout_ms) const;
    // Recovery is attach-only: enumerate existing volumes and adopt exactly
    // one candidate backed by disk_number and mounted at drive_letter.
    [[nodiscard]] Result<ProductVolume> find_recovered_volume(
        std::uint32_t disk_number, wchar_t drive_letter,
        std::wstring owner_token) const;
    [[nodiscard]] Result<VolumeObservation> observe(const ProductVolume& expected) const;
    [[nodiscard]] Result<void> flush(const ProductVolume& owned) const;
    [[nodiscard]] Result<VolumeLockAttempt> lock(
        const ProductVolume& owned, std::uint32_t timeout_ms) const;
    [[nodiscard]] Result<void> assign_drive_letter(
        const ProductVolume& owned, wchar_t drive_letter) const;
    [[nodiscard]] Result<void> remove_drive_letter(
        const ProductVolume& owned) const;
};

// Executes the barrier only when the exact-volume identity is confirmed both
// immediately before and immediately after the flush.  The callbacks are a
// deterministic seam for policy tests; native callers provide extent/owner
// probes tied to the retained volume handle.
using FlushIdentityCheck = std::function<Result<void>()>;
using FlushOperation = std::function<Result<void>()>;
Result<void> flush_with_identity_checks(const FlushIdentityCheck& verify_before,
                                        const FlushOperation& flush,
                                        const FlushIdentityCheck& verify_after);

Result<ProductVolume> provision_new_volume(
    const VolumeObservation& observation,
    const ProductVolume& expected);

Result<ProductVolume> find_recovered_volume(
    std::span<const VolumeObservation> observations,
    const ProductVolume& expected);

// Deterministic selection seam for the native bridge.  Candidates must have
// one exact extent on disk_number and the requested drive-letter ownership;
// zero or ambiguous candidates fail closed.
Result<ProductVolume> select_volume_for_disk(
    std::span<const VolumeObservation> observations,
    std::uint32_t disk_number, wchar_t drive_letter,
    std::wstring owner_token, bool require_mounted);

Result<void> flush_volume(
    const VolumeObservation& observation,
    const ProductVolume& owned);

Result<VolumeLockAttempt> lock_volume(
    const VolumeObservation& observation,
    std::uint32_t timeout_ms);

Result<void> assign_drive_letter(
    const VolumeObservation& observation,
    const ProductVolume& owned,
    wchar_t drive_letter);

Result<void> remove_drive_letter(
    const VolumeObservation& observation,
    const ProductVolume& owned);

} // namespace myrd
