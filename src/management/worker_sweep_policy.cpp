#include "src/management/worker_sweep_policy.h"

namespace myrd::management {

OrphanDisposition classify_orphan(const OrphanedWorker& orphan) noexcept {
    // A pid of zero is not a process.  Reported rather than acted on: a job
    // enumeration that produced one is a broken enumeration, and the safe answer
    // to "I cannot name this process" is to leave the job alone rather than to
    // start terminating things.
    if (orphan.process_id == 0) return OrphanDisposition::indeterminate;

    // No binding we recorded for this process.  It is inside our job object,
    // which is the one thing we know about it, and the job is ours.
    //
    // Terminating it is safe *because* the job is the evidence:
    // `AssignProcessToJobObject` is how a process got here, so a process in this
    // job was started by this service.  What is unknown is only which profile it
    // belonged to, and a worker whose profile we cannot name is a worker no
    // operation can ever address again.
    if (!orphan.profile_id.has_value()) return OrphanDisposition::foreign;

    // A binding with no creation time is half an identity -- a pid, in other
    // words -- and a pid is exactly what Windows recycles.  There is nothing to
    // compare against, so there is nothing to conclude.
    if (!orphan.recorded_creation_time.has_value()) {
        return OrphanDisposition::indeterminate;
    }

    // The load-bearing comparison.  A matching pid with a different creation time
    // is a *different process* that happens to have inherited a dead worker's
    // number; both terminating it and adopting it would be acting on a stranger.
    if (orphan.process_creation_time != *orphan.recorded_creation_time) {
        return OrphanDisposition::pid_recycled;
    }

    // Same pid, same creation time: the process the binding recorded.  The
    // service is starting up, so it is not running this profile's disk any more
    // -- nothing has started it yet in this process -- and what it owes us is the
    // ordered teardown: remove the drive letter, dismount, stop AIM.  A forced
    // kill would let it do none of those, which is why the job does not set
    // `KILL_ON_JOB_CLOSE`.
    return OrphanDisposition::ordered_teardown;
}

std::optional<ObservedResourceIdentity> reclaimable_resources(
    const std::optional<ObservedResourceIdentity>& observed) noexcept {
    // Nothing was ever recorded: the previous service life never got far enough
    // to observe these resources.  That is not "there is nothing left behind",
    // it is "I cannot tell" -- so nothing is reclaimed.  Returning an empty
    // identity here instead would be the single most dangerous shortcut in this
    // file: it would authorise a cleanup with no proof behind it.
    if (!observed.has_value()) return std::nullopt;

    // Recorded but incomplete.  Same answer and for the same reason: a partial
    // identity cannot be matched against anything, so a cleanup driven by it
    // would be matching on whatever part happened to be present -- which is how a
    // sweep starts removing somebody else's volume.
    if (!observed->complete()) return std::nullopt;

    // Complete.  Returned as-is rather than narrowed to the fields the caller
    // needs: what may be reclaimed is the whole identity, and a caller that
    // wanted to reclaim on a subset would have to say so explicitly.
    return observed;
}

ObservedResourceIdentity project_observed_identity(
    const profile::ProfileRecord& record,
    const std::optional<BundleFileIdentity>& bundle_file_identity,
    const std::uint64_t generation) noexcept {
    ObservedResourceIdentity identity{};

    // The bundle, from the two places it is actually recorded.  The image uuid is
    // on the profile record; the directory's own file identity is not, and is
    // absent until a worker has reported it -- which is a real state, not a
    // failure, and `complete()` is what turns it into "do not reclaim".
    //
    // The record holds the product uuid and this field wants the persistence
    // layer's sixteen raw bytes.  They are the same sixteen in the same order, so
    // this is a copy rather than a conversion -- the same reasoning
    // `WindowsWorkerSessionProvider` already documents for the reverse direction,
    // and going through text would be a parse that can only fail.
    {
        persistence::ImageUuid image{};
        const auto bytes = record.profile.image_uuid.bytes();
        static_cast<void>(
            std::memcpy(image.bytes.data(), bytes.data(), image.bytes.size()));
        identity.bundle_image_id = image;
    }
    identity.bundle_file_identity = bundle_file_identity;

    // The volume, by GPT identities.  The durable record carries the *product*
    // uuid form and the worker reports the canonical text form; they are the same
    // identity written two ways, so the record's form is converted rather than a
    // second copy being stored.  Empty when the record has none, which is the
    // "never had a volume" state.
    if (record.profile.volume_guid.has_value()) {
        identity.volume_guid = record.profile.volume_guid->to_string();
    }

    // The mount, and this is the field where the two types disagree in *shape*.
    //
    // `ObservedResourceIdentity::mount_target` is a single `wchar_t` because the
    // reclamation it authorises is "clear this drive letter".  A profile record's
    // `mount_target` is a string, and it can legitimately be a directory (a mount
    // point rather than a letter) -- which cannot be expressed as one character and
    // must therefore NOT be silently truncated to its first one.  Taking `R` out of
    // `R:` would work by accident for letters and would invent a letter for a
    // directory mount, so the conversion accepts only an exact letter form and
    // leaves the field unset otherwise.  Unset means `complete()` is false, which
    // means nothing is reclaimed -- the safe answer for a mount we cannot name.
    if (record.profile.mount_kind == profile::MountKind::drive_letter
        && record.profile.mount_target.size() == 2
        && record.profile.mount_target[1] == L':') {
        const wchar_t letter = record.profile.mount_target[0];
        const bool is_letter = (letter >= L'A' && letter <= L'Z')
            || (letter >= L'a' && letter <= L'z');
        if (is_letter) {
            identity.mount_target =
                (letter >= L'a') ? static_cast<wchar_t>(letter - L'a' + L'A')
                                 : letter;
        }
    }

    identity.generation = generation;
    return identity;
}

} // namespace myrd::management
