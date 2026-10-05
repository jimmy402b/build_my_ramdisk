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
#include "src/product/uuid.h"

#include <cstdint>
#include <memory>
#include <string>

namespace myrd::management {

class WindowsWorkerSessionProvider final : public IWorkerSessionProvider {
public:
    // The executable to start.  Passed in rather than derived: where the worker
    // binary lives is an installation question, and a provider that guessed
    // would be a second place for the answer to be wrong.
    explicit WindowsWorkerSessionProvider(std::wstring worker_executable) noexcept;

    [[nodiscard]] Result<std::unique_ptr<ServiceWorkerSession>> launch(
        const WorkerLaunchRequest& request,
        const OperationContext& context) override;
    [[nodiscard]] Result<std::unique_ptr<ServiceWorkerSession>> attach(
        const product::Uuid& profile_id,
        const OperationContext& context) override;

private:
    std::wstring worker_executable_;
    // One per launched session, so a recycled pipe name can never be mistaken
    // for this session's, and so a worker from a previous service session cannot
    // authenticate against this one.
    std::uint64_t next_session_generation_{1};
};

} // namespace myrd::management
