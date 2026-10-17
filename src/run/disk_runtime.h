#pragma once

#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/run/windows_volume.h"

#include <array>
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

// `reclaim` is the third mode, and it exists because a leftover cannot be recovered.
//
// A worker that was killed rather than asked to leave left a mounted volume with its
// drive letter **already assigned**.  `recover` cannot help: its contract says it
// *"assigns the requested drive letter"*, and the letter it would request is the one the
// residue is holding -- so it collides with itself.  `reclaim` is the mode whose whole
// job is the opposite: adopt the existing volume through the attach-only find, assign
// **nothing**, and hand the runtime straight to teardown.
//
// It is an explicit mode rather than a flag on `recover` because the two differ in what
// they promise, not in how they are parameterized: `recover` brings a profile back into
// service, `reclaim` takes one out of it, and a caller that could not tell them apart
// would be able to mount a volume by accident on a path whose entire purpose is to avoid
// doing so.
enum class DiskRuntimeMode : std::uint8_t { create, recover, reclaim };

enum class DiskCheckpointDisposition : std::uint8_t {
    committed,
    not_committed,
    commit_indeterminate,
    // Appended rather than folded into `commit_indeterminate`.  The two are not
    // the same fact: "the outcome is unknown" has to be parked for
    // reconciliation, while "the bytes landed but the epoch publication did not"
    // is a known state with its own recovery path.  Folding them was a real
    // defect -- the service could not tell which it was being told.
    committed_needs_reconciliation,
};

// The persistence failure that decided a commit which did not cleanly land,
// mirrored from `persistence::PersistenceErrorContext` for the same reason the
// disposition above is mirrored: this header is the engine boundary and stays
// free of M3 and persistence headers.  The shared adapter performs the
// translation, and ServiceCore's `CheckpointPersistenceErrorContext` is the
// management-side mirror of the same record, so all three carry one field set.
struct DiskCheckpointErrorContext final {
    std::string operation;
    std::uint32_t code{};
    std::uint8_t phase{};
    std::uint32_t native_code{};
    std::uint64_t native_operation_id{};
    std::array<std::byte, 16> image_uuid{};
    std::uint64_t generation{};
    DiskCheckpointDisposition disposition{
        DiskCheckpointDisposition::not_committed};
    std::string safe_context;

    bool operator==(const DiskCheckpointErrorContext&) const noexcept = default;
};

struct DiskCheckpointResult final {
    DiskCheckpointDisposition disposition{
        DiskCheckpointDisposition::not_committed};
    std::uint64_t generation{};
    // Present exactly when a commit did not cleanly land.  It is the evidence
    // ServiceCore validates the failure against, so it travels rather than being
    // summarized: a caller that had to re-derive a context would end up naming a
    // persistence failure that never happened.
    std::optional<DiskCheckpointErrorContext> error_context;
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
//
// ── What every implementer owes this contract ────────────────────────────────
//
// **Each callback must be answerable to three questions, and until now none of them was written down:**
//
//   1. **What does it cost, at worst?**  Milliseconds, or unbounded?
//   2. **When may it be called?**  Is there a state the machine must not be in?
//   3. **What does the runtime do with the failure?**  Retry, park, or tear down?
//
// **This is not documentation for its own sake.**  The worst defect this increment produced was a callback
// that answered (1) with "about 2500 microseconds" on an idle machine and with "over eleven seconds" while a
// device was retiring -- and nothing in the contract said a caller had to care.  The same read also showed
// that a bound placed by the caller is useless when the callee can block inside a single call: the budget
// bounded how many times a loop turned, not how long one turn took.
//
// So the convention is:
//
//   - **Bounded** callbacks may be called from any lifecycle step; their cost is expected to be small and
//     independent of machine state.
//   - **Unbounded** callbacks may block indefinitely and must be documented as such.  A caller that has a
//     deadline must not assume it can enforce one, and a caller in a teardown path must prefer an
//     alternative that does not require them.
//   - **State-sensitive** callbacks are only sound in a particular state -- typically "no device is being
//     removed" -- and the caller is responsible for arranging that.
//   - **Ownership** is stated explicitly: which callback establishes it, and which callbacks may destroy
//     what without it.
//
// Comments below mark the callbacks where one of these is not obvious.  The ones left unmarked are bounded,
// state-insensitive and do not touch a device.
struct DiskRuntimeOperations final {
    std::function<Result<void>()> preflight;
    std::function<Result<DiskBundleKind>(const std::filesystem::path&)>
        classify_bundle;
    std::function<Result<void>(const std::filesystem::path&)> acquire_bundle_lock;
    std::function<Result<void>()> release_bundle_lock;

    std::function<Result<void>(std::uint64_t)> create_generation_zero_image;
    std::function<Result<void>(std::uint64_t)> recover_complete_image;
    // **UNBOUNDED and STATE-SENSITIVE historically; now bounded only because the caller arranged it.**
    //
    // Bringing AIM up used to locate the physical drive the new device became, by opening every candidate
    // disk.  Measured: about 2500 microseconds while the machine was idle, and one open at 11017 ms while a
    // device was retiring.  That is the difference between "fast" and "cannot be given up", and it is why the
    // attach-only paths now skip the lookup entirely rather than trusting a deadline.
    //
    // Contract for implementers: if locating a disk is unavoidable, say so here, and state what machine
    // state makes it unsafe.  A caller with a deadline cannot enforce one against a call that blocks inside.
    std::function<Result<void>()> start_aim;
    std::function<Result<void>()> stop_aim;

    std::function<Result<ProductVolume>(std::uint64_t, wchar_t)>
        provision_new_volume;
    // **Selects a volume for adoption.  Bounded, but the selection must be identity-based.**
    //
    // Two things about it were learned the hard way and belong in the contract rather than in one
    // implementation's head:
    //
    //   1. **A drive letter is not an identity.**  The enumerator stamps every volume with the letter it was
    //      asked about, so a letter test alone rejects nothing and a caller can end up choosing among volumes
    //      that merely exist.  Selection has to narrow on something that cannot collide -- a recorded volume
    //      GUID, or the fact that the volume is actually mounted at that letter.
    //   2. **The durable record, when one exists, must be the expectation.**  Comparing the found volume
    //      against itself proves only that it did not change in between, which is not the question that
    //      matters after a power loss.
    //
    // Implementations must therefore accept an expected identity when the caller has one, and must fail
    // closed when several candidates match and nothing distinguishes them.
    std::function<Result<ProductVolume>(wchar_t)> find_recovered_volume;
    std::function<Result<void>(const ProductVolume&, std::uint64_t)>
        verify_volume;
    // Flushes the volume's cached writes.  `held` is the write freeze this
    // runtime already holds, if any, and the flush must go *through it*: the
    // freeze opens the volume for exclusive access, so an implementation that
    // opened its own handle here would be refused by the runtime's own lock.  An
    // empty `held` means there is no freeze and the implementation opens its own,
    // which is what the create path and every M4 caller do.
    std::function<Result<void>(const ProductVolume&,
                               std::optional<LockedVolume>&)>
        flush_volume;
    std::function<Result<void>(const ProductVolume&, wchar_t)>
        assign_drive_letter;
    // Takes and *returns* the volume's exclusive lock, which the runtime then
    // retains.  Ownership travels with the attempt rather than being left with
    // the provider, because the window the lock has to cover is not one call:
    // the stop plan freezes writes, flushes, commits and only then dismantles,
    // and each of those is a separate command.  A lock the provider held would
    // be released the moment its call returned, which is the defect this hook
    // exists to end.
    //
    // Optional so a foreground M4 run -- which takes its own lock through its
    // own hooks and never freezes -- keeps working unchanged.
    std::function<Result<VolumeLockAttempt>(const ProductVolume&, std::uint64_t)>
        lock_volume;
    // Dismantles the volume, given the lock the runtime is holding for it.
    //
    // The lock is *passed*, not re-acquired, because acquiring it a second time
    // is not possible: the provider opens the volume for exclusive access, so a
    // second attempt while this runtime already holds it is refused as busy by
    // this operation's own lock.  Re-acquiring was a real defect -- it made
    // `remove_mount` fail on the freeze it was relying on -- so the fact that the
    // runtime holds a lock is stated here instead of being rediscovered below.
    // An empty optional means the runtime holds no lock; the implementation then
    // takes its own, which is the M4 behaviour and remains correct.
    std::function<Result<void>(const ProductVolume&,
                               std::optional<LockedVolume>&)>
        dismount_volume;
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
    // Adopts an existing, already-mounted volume **without assigning anything**, so the
    // caller can dismantle a leftover.  See `DiskRuntimeMode::reclaim` for why this is a
    // mode of its own rather than `recover` with a flag.
    static Result<std::unique_ptr<DiskRuntime>> reclaim(
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

    // Takes the adopted volume's exclusive lock and keeps it, which is what
    // makes "the writes have stopped" a fact rather than a hope.
    //
    // The M4 shutdown order is `lock -> verify -> flush -> checkpoint ->
    // dismount`, and the lock is what gives the commit its meaning: while the
    // volume is locked the filesystem accepts no further write, so a commit that
    // lands afterwards is the last word on that volume.  Without it a stop can
    // commit, then be written to again, and the removal that follows is refused
    // because the runtime it is refusing over is genuinely dirty again.
    //
    // The lock is held by this runtime, not by the caller, because the window it
    // must cover spans flush, checkpoint and removal -- three separate commands
    // with three separate callers.  It is released by `release_write_freeze`,
    // which the teardown runs once the volume is dismantled.
    //
    // Idempotent: a second call succeeds and keeps the lock already held.  That
    // is deliberate rather than lenient -- taking a second lock is not possible
    // (the provider opens the volume exclusively), so "freeze again" is only ever
    // a request for a state the runtime is already in, and failing it would turn
    // a retry of an already-satisfied precondition into a stop that cannot
    // proceed.
    //
    // A busy volume is a failure, not a silent downgrade: continuing unfrozen
    // would reinstate exactly the window this call exists to close, and the
    // caller must be able to retry a stop rather than discover later that its
    // commit was not the last write.
    Result<void> freeze_writes();
    [[nodiscard]] bool writes_frozen() const;
    // Releases the write freeze if one is held.  Idempotent, and deliberately
    // `noexcept`: every failure path of a teardown has to be able to run it.
    Result<void> release_write_freeze() noexcept;
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
    // The freeze release without the runtime mutex, for the teardown that is
    // already holding it.  Separate from the public call so the two cannot drift
    // into a self-deadlock on one runtime mutex.
    void release_write_freeze_locked() noexcept;

    DiskRuntimeMode mode_;
    DiskRuntimeOptions options_;
    DiskRuntimeOperations operations_;
    std::optional<ProductVolume> volume_;
    bool bundle_lock_held_{};
    bool aim_started_{};
    // Whether this runtime *created* the mount, as opposed to adopting one that was
    // already there.  Kept because a caller can legitimately ask who established a mount.
    bool drive_letter_assigned_{};
    // Whether a drive letter is mounted on `volume_` **right now**, whoever put it there.
    //
    // The flag teardown actually needs, and separating it from the one above is a
    // correction: teardown gated on `drive_letter_assigned_`, which is false in `reclaim`
    // mode, so a reclaim adopted the residue, removed nothing, and reported success.  The
    // two questions -- "did I create this?" and "is a letter mounted here?" -- have
    // different answers on exactly the path that matters.
    bool drive_letter_present_{};
    bool volume_dismounted_{};
    bool started_{};
    bool cleanup_attempted_{};

    mutable std::mutex mutex_;
    // Valid together: the prepared candidate and the native operation id that
    // commit/abort must repeat to publish or release it.
    std::optional<DiskCheckpointPrepared> pending_checkpoint_;
    std::uint64_t pending_native_operation_id_{};
    // The retained write freeze.  Owned here rather than by the provider so its
    // lifetime -- and with it the window in which the volume accepts no write --
    // is the runtime's decision and not one function's scope.
    std::optional<LockedVolume> held_volume_lock_;
    // Whether writes are frozen, tracked apart from the descriptor above.
    //
    // The two are not the same fact, and conflating them was a real defect: the
    // teardown *hands the descriptor to the dismantle*, so the descriptor is
    // empty while the volume is still frozen and still mounted.  A runtime that
    // reported "not frozen" at that moment would tell the worker its volume was
    // writable during the one step where the lock is doing its whole job.
    bool writes_frozen_{};
};

} // namespace myrd
