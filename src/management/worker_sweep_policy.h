#pragma once

// The decisions a startup sweep makes, with no Win32 in them.
//
// A service that restarts has to answer two questions about the workers and the
// devices its previous life left behind: which of these are mine, and which of
// these may I clean up?  Both answers are dangerous to get wrong in the same
// direction -- cleaning up a resource that belongs to somebody else, or to a
// profile that is still legitimately running -- so both are decided here, as
// pure functions over values, and tested directly.
//
// The Win32 side (enumerating a job object, reading a process creation time,
// opening a job) lives behind `WorkerProcessRegistry`; the *policy* lives here.
// Splitting them is what makes "the sweep would have cleaned up a foreign disk"
// an ordinary failing test rather than something only a VM experiment could show.
//
// Two rules from the design are load-bearing and are enforced by construction
// below rather than by convention:
//
//   * **identity, never a name.**  A sweep that matched on process name, AIM
//     device number or display name would eventually clean up something it did
//     not own.  Every decision here is a comparison of a *persisted identity*
//     against an *observed identity*, and "the identities are absent" is an
//     answer -- "leave it alone" -- not a licence.
//   * **a pid is not an identity.**  Windows recycles pids, so a pid alone will
//     eventually name a different process.  A worker is this service's only when
//     the pid *and* the process creation time match what the service recorded,
//     which is the same reason `WorkerBinding` carries both.

#include "src/common/result.h"
#include "src/management/service_core.h"
#include "src/persistence/image_inspector.h"
#include "src/product/uuid.h"

#include <cstdint>
#include <optional>
#include <string>

namespace myrd::management {

// What the service last observed about one profile's resources, as it was
// persisted before the process that observed them died.
//
// This is the *only* thing that may justify cleaning up a residual resource.
// Every field is an identity the worker itself reported (via
// `WorkerBundleEvidence` and the volume identities `WorkerRuntimeStatus`
// carries), never something the service inferred: a service that computed an
// identity would be proving ownership against its own guess.
struct ObservedResourceIdentity final {
    // The bundle, by image uuid and directory file identity.  The file identity
    // is what makes it *this* directory rather than one that replaced it.
    std::optional<persistence::ImageUuid> bundle_image_id;
    std::optional<BundleFileIdentity> bundle_file_identity;
    // The mounted volume, by GPT identities.  Empty means the worker never
    // reported them, which — see below — is "cannot prove ownership".
    std::string volume_guid;
    // The mount this profile was given.  Kept because a residual drive letter is
    // the one leftover that is user-visible: a letter that stays assigned after
    // the volume is gone is a disk that looks present and is not.
    wchar_t mount_target{};
    // The generation the worker stood at.  Recorded so a sweep can say what the
    // profile was doing when it died, and so a reclamation is not performed
    // against a generation the profile has since moved past.
    std::uint64_t generation{};

    // Whether these observations are enough to justify a cleanup.  All three
    // identity classes must be present: a partial identity is not a weaker
    // proof, it is no proof — the same rule `WorkerVolumeIdentity::complete`
    // and `VolumePersistentIdentity::complete` already enforce one layer up.
    [[nodiscard]] bool complete() const noexcept {
        return bundle_image_id.has_value() && bundle_file_identity.has_value()
            && !volume_guid.empty() && mount_target != 0;
    }

    bool operator==(const ObservedResourceIdentity&) const noexcept = default;
};

// One process the previous service life left inside the worker job object.
struct OrphanedWorker final {
    std::uint32_t process_id{};
    // The creation time *observed* for that pid, read from the live process.
    std::uint64_t process_creation_time{};
    // The profile this worker was bound to, when the binding was one this
    // service recorded.  Absent means the job holds a process whose binding this
    // service does not recognise.
    std::optional<product::Uuid> profile_id;
    // The creation time the service *recorded* for that binding.  Present
    // whenever `profile_id` is, because they are recorded together -- a binding
    // without a creation time would be a pid, and a pid is not an identity.
    std::optional<std::uint64_t> recorded_creation_time;
};

enum class OrphanDisposition : std::uint8_t {
    // Not this service's worker, or bound to a profile this service does not
    // serve.  Cooperatively terminated, escalating to a forced termination if it
    // will not go: an unknown process inside our own job is not something to
    // reason about, it is something to remove.
    foreign = 0,
    // Our worker, for a profile we know, and no longer wanted.  Cooperatively
    // terminated so it can run its ordered teardown — which is the whole reason
    // the job does not set `KILL_ON_JOB_CLOSE`.
    ordered_teardown,
    // Our worker, for a profile we know, and it is *not* the process the binding
    // recorded.  Windows recycles pids, so a pid that matches while the creation
    // time does not is a different process wearing a dead worker's number.
    // Terminating it would be killing a stranger, and adopting it would be
    // trusting one.
    pid_recycled,
    // Cannot be classified at all — a zero pid, or a binding with no creation
    // time to compare against.  Left alone and reported: "I cannot tell" is not
    // a licence to terminate, and a sweep that guessed here would eventually
    // guess wrong about a process it does not own.
    indeterminate,
};

// What to do with one orphaned worker.
[[nodiscard]] OrphanDisposition classify_orphan(
    const OrphanedWorker& orphan) noexcept;

// Which leftover resources of one profile may be reclaimed.
//
// `std::nullopt` is the answer that matters most: it means the identity was
// incomplete, so nothing may be reclaimed and the profile is reported
// `needs_attention` with its evidence retained.  A caller cannot accidentally
// treat "unknown" as "nothing to do" -- the type makes it choose.
[[nodiscard]] std::optional<ObservedResourceIdentity> reclaimable_resources(
    const std::optional<ObservedResourceIdentity>& observed) noexcept;

} // namespace myrd::management
