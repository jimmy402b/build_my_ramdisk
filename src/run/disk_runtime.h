#pragma once

#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/run/windows_volume.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace myrd {

enum class DiskBundleKind : std::uint8_t {
    empty,
    manifest,
    unknown_nonempty,
};

enum class DiskRuntimeMode : std::uint8_t { create, recover };

enum class DiskCheckpointDisposition : std::uint8_t {
    committed,
    not_committed,
    commit_indeterminate,
};

struct DiskCheckpointResult final {
    DiskCheckpointDisposition disposition{
        DiskCheckpointDisposition::not_committed};
    std::uint64_t generation{};
};

struct DiskMaintenanceStats final {
    std::uint64_t dirty_bytes{};
    std::uint64_t journal_bytes{};
    std::uint64_t generation{};
};

// M5 two-phase checkpoint boundary (design spec 3.3/3.4).
//
// These mirror the M3 types on purpose, exactly as DiskCheckpointResult already
// mirrors persistence::CommitDisposition: DiskRuntime is the engine boundary and
// stays free of M3 headers.  The shared adapter performs the translation.
//
// The candidate is frozen during prepare.  Commit must repeat the identical
// native id, manifest slot and digest or the prepare stays unpublished, so a
// caller can never publish bytes it did not first observe.
struct DiskCheckpointPrepareRequest final {
    std::uint64_t native_operation_id{};
};

struct DiskCheckpointPrepared final {
    std::uint64_t prior_generation{};
    std::uint64_t candidate_generation{};
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};
};

struct DiskCheckpointCommitRequest final {
    std::uint64_t native_operation_id{};
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};
};

struct DiskRuntimeOptions final {
    std::filesystem::path bundle;
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    std::chrono::milliseconds volume_timeout{30000};
};

// T06 deliberately exposes callbacks instead of production objects.  This
// keeps lifecycle tests deterministic and lets T07 adapt the already accepted
// M3 and Windows-volume implementations without changing their contracts.
struct DiskRuntimeOperations final {
    std::function<Result<void>()> preflight;
    std::function<Result<DiskBundleKind>(const std::filesystem::path&)>
        classify_bundle;
    std::function<Result<void>(const std::filesystem::path&)> acquire_bundle_lock;
    std::function<Result<void>()> release_bundle_lock;

    std::function<Result<void>(std::uint64_t)> create_generation_zero_image;
    std::function<Result<void>(std::uint64_t)> recover_complete_image;
    std::function<Result<void>()> start_aim;
    std::function<Result<void>()> stop_aim;

    std::function<Result<ProductVolume>(std::uint64_t, wchar_t)>
        provision_new_volume;
    std::function<Result<ProductVolume>(wchar_t)> find_recovered_volume;
    std::function<Result<void>(const ProductVolume&, std::uint64_t)>
        verify_volume;
    std::function<Result<void>(const ProductVolume&)> flush_volume;
    std::function<Result<void>(const ProductVolume&, wchar_t)>
        assign_drive_letter;
    std::function<Result<void>(const ProductVolume&)> dismount_volume;
    std::function<Result<void>(const ProductVolume&)> remove_drive_letter;

    std::function<Result<DiskCheckpointResult>()> checkpoint;
    std::function<DiskMaintenanceStats()> maintenance_stats;

    // M5-only two-phase boundary.  A foreground M4 run leaves these unset and
    // keeps using the one-shot checkpoint() above; the per-profile worker
    // supplies them through the shared production adapter.
    std::function<Result<DiskCheckpointPrepared>(
        const DiskCheckpointPrepareRequest&)>
        prepare_checkpoint;
    std::function<Result<DiskCheckpointResult>(
        const DiskCheckpointCommitRequest&)>
        commit_checkpoint;
    std::function<Result<void>(std::uint64_t native_operation_id)>
        abort_checkpoint;
};

class DiskRuntime final {
public:
    static Result<std::unique_ptr<DiskRuntime>> create(
        DiskRuntimeOptions options, DiskRuntimeOperations operations);
    static Result<std::unique_ptr<DiskRuntime>> recover(
        DiskRuntimeOptions options, DiskRuntimeOperations operations);

    ~DiskRuntime() noexcept;
    DiskRuntime(const DiskRuntime&) = delete;
    DiskRuntime& operator=(const DiskRuntime&) = delete;

    // Performs the mode-specific lifecycle. Create must establish generation
    // zero, provision and verify the new volume, flush it, then commit gen 1.
    // Recover must never provision: it recovers M3 using options_.virtual_bytes,
    // finds an existing volume, verifies its geometry against that same size,
    // then assigns the requested drive letter.
    //
    // A failed stage owns no later resources. Cleanup is reverse-order and
    // idempotent: remove a drive letter only when this runtime assigned it,
    // dismount only when this runtime obtained a volume, stop AIM only after
    // AIM started, and release the bundle lock only after lock acquisition.
    Result<void> start();
    [[nodiscard]] DiskMaintenanceStats maintenance_stats() const;
    // The volume this runtime actually adopted, so the service can record its
    // persistent identities into the durable profile.  Absent before a
    // successful start, and absent again once teardown has released it: the
    // identity of a volume the runtime no longer holds is not a fact it still
    // has, and reporting a stale one would let a durable profile name a volume
    // nobody owns.
    [[nodiscard]] std::optional<ProductVolume> adopted_volume() const;
    // Flushes the mounted Windows volume before a persistence checkpoint.
    // This is intentionally separate from checkpoint() so the M4 run loop
    // can enforce the filesystem durability ordering.
    Result<void> flush_volume();
    // May only be called after start() succeeds. The provider's disposition is
    // returned verbatim, including not_committed and commit_indeterminate;
    // callers must not infer commitment from a successful API call alone.
    Result<DiskCheckpointResult> checkpoint();

    // M5 two-phase checkpoint.  All four calls, the one-shot checkpoint() above
    // and the shutdown path below share one runtime mutex, so a service commit
    // can never advance the same transaction as a timeout-driven abort.
    //
    // prepare_checkpoint freezes the candidate and returns its slot and digest.
    // commit_checkpoint publishes only that exact candidate: a request whose
    // native id, slot or digest differs is refused and leaves the prepare
    // pending.  abort_checkpoint releases an unpublished prepare.
    Result<DiskCheckpointPrepared> prepare_checkpoint(
        const DiskCheckpointPrepareRequest& request);
    Result<DiskCheckpointResult> commit_checkpoint(
        const DiskCheckpointCommitRequest& request);
    Result<void> abort_checkpoint(std::uint64_t native_operation_id);
    // True while a prepare has been accepted and not yet resolved by
    // commit/abort.  A worker uses this to decide whether shutdown must abort
    // rather than stop M3 blindly (design spec 3.5).
    [[nodiscard]] bool checkpoint_prepared() const;

    // May only be called after a successful start (or after a failed start has
    // completed its owned-resource cleanup). Repeated calls are harmless.
    Result<void> shutdown_clean();

private:
    DiskRuntime(DiskRuntimeMode mode, DiskRuntimeOptions options,
                DiskRuntimeOperations operations);

    // Releases an unpublished prepare before M3 is stopped.  Returns the abort
    // failure when the transaction could not be resolved, in which case the
    // caller must retain the runtime rather than stop it.
    Result<void> abort_pending_checkpoint_locked();
    Result<void> shutdown_clean_locked();

    DiskRuntimeMode mode_;
    DiskRuntimeOptions options_;
    DiskRuntimeOperations operations_;
    std::optional<ProductVolume> volume_;
    bool bundle_lock_held_{};
    bool aim_started_{};
    bool drive_letter_assigned_{};
    bool volume_dismounted_{};
    bool started_{};
    bool cleanup_attempted_{};

    mutable std::mutex mutex_;
    // Valid together: the prepared candidate and the native operation id that
    // commit/abort must repeat to publish or release it.
    std::optional<DiskCheckpointPrepared> pending_checkpoint_;
    std::uint64_t pending_native_operation_id_{};
};

} // namespace myrd
