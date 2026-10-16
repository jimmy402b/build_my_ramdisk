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
struct WorkerFingerprintRecord final {
    product::Uuid profile_id;
    // The worker process this profile was last observed running, and the creation
    // time that makes the pid an identity rather than a number.
    std::uint32_t worker_pid{};
    std::uint64_t worker_process_creation_time{};
    // The generation the worker stood at when it was recorded, so a later reader
    // can say what the profile was doing.
    std::uint64_t generation{};

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

    // Records what was observed.  Replaces any earlier record for the profile --
    // the fingerprint is "the last worker this profile had", not a history.
    [[nodiscard]] Result<void> record(const WorkerFingerprintRecord& entry);

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
