#pragma once

// The durable "last observed worker fingerprint" per profile.
//
// Design spec section 5.5 asks for this and gives the reason: a worker killed by
// a crash can leave an AIM device, a volume and a drive letter behind, and the
// service may only reclaim what it can *prove* is its own.  The proof is a
// recorded identity, so the fingerprint has to outlive the process that observed
// it.
//
// The sweep needs two different things from it, and only one of them is about
// reclamation:
//
//   * the **worker fingerprint** (pid and process creation time) is what lets the
//     sweep tell "the worker I launched" from "a process that inherited its pid".
//     Without it, every orphan is unattributable and the sweep can only report
//     them -- which is exactly what it did before this store existed;
//   * the **resource identity** (bundle file identity, volume GUID, mount target)
//     is what a later slice uses to reclaim the device and drive letter.  It is
//     recorded here because it is observed at the same moment and splitting the
//     two records would let them disagree.
//
// Two decisions worth stating:
//
//   * **a missing record is not an empty record.**  "I have no fingerprint for
//     this profile" and "this profile has no worker" are different answers, and
//     the sweep treats the first as unattributable rather than as permission;
//   * **the store is fail-closed on read.**  A record that cannot be parsed is
//     reported, not skipped: skipping it would turn a corrupt file into a
//     termination of the very process it was supposed to identify.

#include "src/common/result.h"
#include "src/management/worker_sweep_policy.h"
#include "src/product/uuid.h"
#include "src/profile/profile_store.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::management {

// One profile's recorded fingerprint.
//
// Field order is load-bearing for this struct because it is aggregate-initialized
// positionally throughout the tests: `bundle_file_identity` is **last** so the four
// fields that existed before it keep their positions, and an initializer that names
// only those four keeps its meaning.  Inserting it earlier silently reinterpreted
// every such initializer -- which is exactly the kind of change the project's
// "appended so existing aggregate initialization keeps its meaning" convention exists
// to prevent.
struct WorkerFingerprintRecord final {
    product::Uuid profile_id;
    // The worker process this profile was last observed running, and the creation
    // time that makes the pid an identity rather than a number.
    std::uint32_t worker_pid{};
    std::uint64_t worker_process_creation_time{};
    // The generation the worker stood at when it was recorded, so a later reader
    // can say what the profile was doing.
    std::uint64_t generation{};
    // The bundle directory's own identity, and the field that decides whether a
    // leftover volume may be reclaimed at all.
    //
    // It cannot be filled in at launch: the worker learns it when it opens and
    // verifies the bundle, and reports it in its *status*.  So a record written at
    // launch is legitimately incomplete here, and
    // `ObservedResourceIdentity::complete()` turns that into "do not reclaim" --
    // which is why `amend_bundle_identity` exists, to fill it in once the worker has
    // said what the identity is.
    //
    // NOT back-filled from the profile record, because the profile record does not
    // have it: the identity travels with the bundle registration evidence, and the
    // only place it is durable today is a removal tombstone.
    std::optional<BundleFileIdentity> bundle_file_identity;

    bool operator==(const WorkerFingerprintRecord&) const noexcept = default;
};

class WorkerBindingStore final {
public:
    // Opens (or creates) `<root>\MyRamDisk\workers`.  Creation is deliberate here
    // and is *not* a contradiction of the service's fail-closed rule for the
    // product root: that rule is about the durable roots the installer owns, and
    // this is a leaf the service owns inside them.  The installer creates the
    // parent, and a service that could not write its own fingerprints would be
    // unable to sweep at all.
    [[nodiscard]] static Result<WorkerBindingStore> open(
        std::wstring_view trusted_program_data_root);

    WorkerBindingStore(WorkerBindingStore&&) noexcept;
    WorkerBindingStore& operator=(WorkerBindingStore&&) noexcept;
    ~WorkerBindingStore() noexcept;

    WorkerBindingStore(const WorkerBindingStore&) = delete;
    WorkerBindingStore& operator=(const WorkerBindingStore&) = delete;

    // Over an already-open directory, bypassing the ACL-protected root.
    //
    // This exists so the *encoding* of the fingerprint -- which is the part that
    // decides whether a crashed worker can be attributed -- is testable on a
    // machine where the protected ProgramData root cannot be created.  It is not a
    // second production path: `open` is the only way the service builds one, and
    // nothing in `src/` calls this.
    [[nodiscard]] static WorkerBindingStore over_directory(
        std::unique_ptr<profile::IAtomicFileDirectory> directory) noexcept;

    // The same, over a *real* managed directory the caller opened.
    //
    // Two factories rather than one because they measure different things, and the
    // distinction cost a round: `over_directory` takes a fake and proves the
    // encoding, while this one proves the four-step atomic publish against the real
    // Windows directory -- which is where a publish that only ever worked against
    // an in-memory map would fail on the guest.  An integration test needs this
    // one; a unit test cannot use it, because creating the protected root is
    // exactly what a development machine cannot do.
    [[nodiscard]] static Result<WorkerBindingStore> over_protected_root(
        std::wstring_view trusted_program_data_root);

    // Records what was observed.  Replaces any earlier record for the profile --
    // the fingerprint is "the last worker this profile had", not a history.
    [[nodiscard]] Result<void> record(const WorkerFingerprintRecord& entry);

    // The bundle directory's identity, as the worker reported it.
    //
    // A separate call from `record` because the two facts arrive at different times:
    // the process at launch, the bundle identity only once the worker has opened and
    // verified it.  Amending rather than replacing, so a status observed after launch
    // cannot erase the pid that makes the process attributable.
    //
    // Returns whether there *was* a record to amend.  A worker's status can arrive for
    // a profile whose launch this service never recorded -- after a restart, or for a
    // profile another session created -- and inventing a record from a status would
    // produce a fingerprint with no process identity, which attributes nothing and
    // looks like it attributes something.
    [[nodiscard]] Result<bool> amend_bundle_identity(
        const product::Uuid& profile_id,
        const BundleFileIdentity& bundle_file_identity);

    // Removes one profile's fingerprint.  Called when a profile is removed or
    // forgotten: a stale fingerprint would attribute a *later*, unrelated process
    // to a profile that no longer exists.
    [[nodiscard]] Result<void> forget(const product::Uuid& profile_id);

    // One profile's fingerprint, or `nullopt` when none was ever recorded.  The
    // distinction the header describes is preserved by the return type.
    [[nodiscard]] Result<std::optional<WorkerFingerprintRecord>> find(
        const product::Uuid& profile_id) const;

    // Every recorded fingerprint.  A corrupt record fails the whole call rather
    // than being dropped, because a caller that silently lost one would go on to
    // treat that profile's worker as unattributable without knowing why.
    [[nodiscard]] Result<std::vector<WorkerFingerprintRecord>> list() const;

private:
    class Impl;
    explicit WorkerBindingStore(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

} // namespace myrd::management
