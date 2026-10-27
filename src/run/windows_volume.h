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

// The persistent GPT identities of the volume an observation adopted.
//
// These are the identities the M5 service records into the durable profile and
// re-checks on every later start, so they have to come from the volume itself
// and never from a caller-supplied value.  They are canonical lowercase GUID
// text with no braces because `src/run` deliberately has no dependency on
// `src/product`, and `product::Uuid` exposes no byte-level factory -- the one
// canonical parser is `product::Uuid::parse`, so the layer that needs a `Uuid`
// parses this text there rather than a second conversion living here.
//
// `complete()` is the single gate.  Two of three is not a weaker identity: it is
// one that cannot be compared against a durable profile at all, so every caller
// treats an incomplete triple exactly like an absent one.
struct VolumePersistentIdentity final {
    std::string disk_guid;
    std::string partition_guid;
    std::string volume_guid;

    [[nodiscard]] bool complete() const noexcept {
        return !disk_guid.empty() && !partition_guid.empty()
            && !volume_guid.empty();
    }
    bool operator==(const VolumePersistentIdentity&) const noexcept = default;
};

// Formats the sixteen bytes of a Win32 GUID, in their in-memory layout, as
// canonical lowercase registry text.  Data1, Data2 and Data3 are little-endian
// in memory while Data4 is already in text order, so this is a field-wise read:
// a plain byte-wise hex dump yields a GUID that looks plausible, parses fine,
// and never matches the one Windows reports for the same object.
std::string canonical_guid_text(std::span<const std::byte, 16> guid_bytes);

struct ProductVolume final {
    std::wstring owner_token;
    std::wstring volume_guid;
    std::vector<VolumeExtent> extents;
    wchar_t drive_letter{};
    bool mounted{};
    bool operation_owned{};
    // Declared last so every existing aggregate initializer keeps meaning what
    // it meant: a fixture that does not name it gets an empty identity, which
    // `complete()` reports as "not established" rather than as a real one.
    VolumePersistentIdentity identity;
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

    // Adopts a handle that already holds the volume's lock, with the identity it
    // was verified against.
    //
    // Public because the lock's *lifecycle* is what the engine layer reasons
    // about -- `DiskRuntime` retains one across several commands -- and a fake
    // provider on that seam has to be able to produce one.  Production reaches
    // this through `Win32VolumeAdapter::lock`, which is still the only thing that
    // can obtain a handle from a device.  Reaching it directly from production
    // code would mean adopting a lock nobody took, which is why every caller is
    // a lock result or a test that never dereferences the handle.
#ifdef _WIN32
    explicit LockedVolume(HANDLE handle, ProductVolume identity) noexcept
        : handle_(handle), identity_(std::move(identity)) {}
#else
    explicit LockedVolume(void* handle, ProductVolume identity) noexcept
        : handle_(handle), identity_(std::move(identity)) {}
#endif

private:
    friend class Win32VolumeAdapter;
#ifdef _WIN32
    HANDLE handle_{INVALID_HANDLE_VALUE};
#else
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
    // The Win32 error that decided a refusal, when one did.
    //
    // Carried because "busy" is a decision, not a cause, and the cause is what a
    // caller acts on: a sharing violation is another process using the disk,
    // `ERROR_NOT_READY` is a volume still settling, and an access denial is a
    // permissions problem.  It is not part of `operator==` on purpose -- equality
    // is about what the attempt *decided*, and two refusals for different native
    // reasons are still the same decision.
    //
    // Declared last so every existing aggregate initializer keeps its meaning.
    std::uint32_t native_code{};

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
    // one candidate mounted at drive_letter.
    //
    // **`expected_guid` is the durable identity, and it is what actually disambiguates.**
    //
    // Two defects made it necessary.  `enumerate_native_volumes` stamps every volume with the
    // *requested* drive letter, so the letter filter in `select_volume_for_disk` rejects nothing; and
    // `verify_volume` compares the volume against the GUID `find` just returned, which came off the same
    // volume, so it never consults the durable record.  Together they meant the physical-disk number was
    // the only real discriminator, and that number is what we removed.
    //
    // A GUID is the right replacement: Windows reuses drive numbers, it does not reuse volume GUIDs, and
    // unlike a letter it survives the volume being unmounted -- which recovery after power loss must
    // handle.
    //
    // Absent means "not known here" -- the foreground `myrd run` path has no durable profile.  Then the
    // previous behaviour stands and the caller accepts that ambiguity is possible.
    [[nodiscard]] Result<ProductVolume> find_recovered_volume(
        std::optional<std::uint32_t> disk_number, wchar_t drive_letter,
        std::wstring owner_token,
        std::optional<std::wstring> expected_guid = std::nullopt) const;
    [[nodiscard]] Result<VolumeObservation> observe(const ProductVolume& expected) const;
    [[nodiscard]] Result<void> flush(const ProductVolume& owned) const;
    [[nodiscard]] Result<VolumeLockAttempt> lock(
        const ProductVolume& owned, std::uint32_t timeout_ms) const;
    [[nodiscard]] Result<void> assign_drive_letter(
        const ProductVolume& owned, wchar_t drive_letter) const;
    [[nodiscard]] Result<void> remove_drive_letter(
        const ProductVolume& owned) const;
    // Writes the volume's label (T4-02).  Separate from assign_drive_letter and additive on purpose: the label
    // is metadata the caller may or may not want, and folding it into the mount would change a signature whose
    // callers have no opinion about labels.
    [[nodiscard]] Result<void> apply_volume_label(
        wchar_t drive_letter, const std::wstring& label) const;
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
// one exact extent on disk_number -- when one is given -- and the requested
// drive-letter ownership; zero or ambiguous candidates fail closed.
//
// `nullopt` for `disk_number` means "do not filter by physical disk", which is
// what recovery and reclaim now pass: see `find_recovered_volume` above for why
// the drive number is no longer worth obtaining, and what still identifies the
// volume without it.
Result<ProductVolume> select_volume_for_disk(
    std::span<const VolumeObservation> observations,
    std::optional<std::uint32_t> disk_number, wchar_t drive_letter,
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
