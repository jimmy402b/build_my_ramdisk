#pragma once

// The production `IWorkerSessionProvider`: it turns a launch request into a
// launch spec, starts a worker process, and hands back the authenticated
// session.
//
// A rebuild, not a port.  The historical provider reached HostCore's own
// launcher; this one is a thin mapping from `WorkerLaunchRequest` to
// `m5::WorkerLaunchSpec` over `launch_private_worker`, which owns the pipe, the
// startup channel and the handshake.  What is left here is the translation to
// the wire, and it is deliberately the only part that knows the wire: a launch
// request is produced by `make_launch_request`, so the two producers and this
// one agree about what a launch is without either of them knowing a profile.

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/management/service_worker_control.h"
#include "src/management/worker_process_source.h"
#include "src/product/uuid.h"

#include <cstdint>
#include <memory>
#include <string>

namespace myrd::management {

// Forward declared rather than included: this header only needs the name for a
// reference, and `windows_worker_job.h` includes this one's interface -- so
// including it back would be a cycle.  The provider never touches the job
// directly; it hands the handle to the launcher and the process to the registry.
class WindowsWorkerProcessSource;

class WindowsWorkerSessionProvider final : public IWorkerSessionProvider {
public:
    // The executable to start.  Passed in rather than derived: where the worker
    // binary lives is an installation question, and a provider that guessed
    // would be a second place for the answer to be wrong.
    //
    // `processes` is the worker job and its live registry.  The provider needs it
    // for two things that must not be separable: every launch is placed into the
    // job at creation, and the process handle is *retained* there so the sweep can
    // find the worker after a crash and so a termination can wait for it.  A
    // provider without one would launch workers nothing could ever account for, so
    // it is a constructor parameter rather than an optional setter.
    WindowsWorkerSessionProvider(std::wstring worker_executable,
                                 WindowsWorkerProcessSource& processes) noexcept;

    [[nodiscard]] Result<std::unique_ptr<ServiceWorkerSession>> launch(
        const WorkerLaunchRequest& request,
        const OperationContext& context) override;
    [[nodiscard]] Result<std::unique_ptr<ServiceWorkerSession>> attach(
        const product::Uuid& profile_id,
        const OperationContext& context) override;

private:
    std::wstring worker_executable_;
    // Borrowed for the provider's whole life, and must outlive it: it owns the
    // job the workers are created into and the handles that keep them reachable.
    WindowsWorkerProcessSource& processes_;
    // One per launched session, so a recycled pipe name can never be mistaken
    // for this session's, and so a worker from a previous service session cannot
    // authenticate against this one.
    std::uint64_t next_session_generation_{1};
};

} // namespace myrd::management
