#pragma once

// Maps the worker's own runtime status onto the management-side observation the
// read path consumes.
//
// A rebuild, not a port.  The historical file of this name translated
// `host::WorkerRuntimeStatus`, which the management core is forbidden to link;
// this one translates `m5::WorkerRuntimeStatus`, which it already does link
// through the private protocol.  What it keeps is the *shape* and the reason for
// it: this is the one place allowed to see both sides, and confining the
// translation here leaves the read path with a single seam.
//
// Header-only on purpose: the translation is one pure function with no state, so
// a translation unit would add a build entry without adding a place for a defect
// to hide.

#include "src/host/m5_worker_protocol.h"
#include "src/management/service_core.h"

namespace myrd::management {

// `session_alive` is passed separately rather than read from the status, because
// the two describe different things and only one of them is in the status at
// all.  The caller knows whether an authenticated session answered just now; the
// status says whether the *runtime* behind it is up, which a worker that has
// cleaned up will report as false while still answering commands.  Both are
// needed: a profile whose worker is running but whose runtime was torn down is
// not a running disk, and reporting it as one is how a stopped profile goes on
// reading as live.
inline WorkerRuntimeObservation observe_worker_runtime(
    const m5::WorkerRuntimeStatus& status,
    const bool session_alive) noexcept {
    WorkerRuntimeObservation observation{};
    observation.worker_alive = session_alive && status.worker_alive;
    observation.committed_generation = status.committed_generation;
    observation.dirty = status.dirty;
    observation.dirty_epoch = status.dirty_epoch;
    // Only meaningful while a worker is alive: a torn-down runtime's last active
    // operation is history, not a fact about what is running, and carrying it
    // would name an operation as in-flight after it can no longer be.
    if (observation.worker_alive) {
        observation.active_operation_id = status.active_operation_id;
        observation.active_operation_phase = status.active_operation_phase;
    }
    return observation;
}

} // namespace myrd::management
