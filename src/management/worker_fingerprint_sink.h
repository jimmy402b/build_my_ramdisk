#pragma once

// Where the service records a bundle's identity, once a worker has read it.
//
// This is design section 5.5's "persist the last-observed identity" seen from the
// **producing** side.  The consumer is the startup sweep, which reads the identity back
// to decide whether a leftover volume may be reclaimed; `WorkerBindingStore` is the
// durable record both sides meet at.
//
// It lives in its own header because the two ends are in different files that would
// otherwise include each other: the store implements this, and
// `ServiceWorkerControl` calls it.  A small interface is also the honest description of
// the dependency -- the command adapter needs one call, not durable state.
//
// **The call is best-effort and its failure is not fatal to the operation that makes
// it.**  A bundle that was created and verified is a real, useful disk; refusing it
// because a *fingerprint* could not be written would trade a working disk for a
// bookkeeping problem.  What a failure costs is bounded and visible: the profile's
// leftovers become unprovable, and the next start's sweep reports that instead of
// reclaiming them.

#include "src/common/result.h"
#include "src/management/service_core.h"
#include "src/product/uuid.h"

namespace myrd::management {

class IWorkerFingerprintSink {
public:
    virtual ~IWorkerFingerprintSink() noexcept = default;

    // Amends an existing fingerprint with the bundle identity.
    //
    // Returns whether there was a record to amend.  A worker's evidence can arrive for
    // a profile whose launch this service never recorded -- after a restart, or for a
    // profile another session created -- and **inventing a fingerprint from evidence
    // would produce a record with no process identity**: one that attributes nothing
    // while looking like it attributes something.
    [[nodiscard]] virtual Result<bool> amend_bundle_identity(
        const product::Uuid& profile_id,
        const BundleFileIdentity& bundle_file_identity) = 0;
};

} // namespace myrd::management
