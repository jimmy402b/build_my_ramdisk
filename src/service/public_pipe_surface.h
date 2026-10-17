#pragma once

// The public pipe, as the host's `IPublicCommandSurface`.
//
// One object owns the pipe server and the endpoint over the same core, because the
// two have to be opened and closed together: a server without an endpoint accepts
// connections and then has no handler, and an endpoint without a server is a command
// surface nobody can reach.  `ServiceHost` decides *when*; this decides *what*.
//
// The server takes the pipe name as a parameter for the same reason the client does:
// so an end-to-end run can use a private pipe instead of competing with an installed
// service.  Production passes nothing and gets the documented name.

#include "src/common/result.h"
#include "src/control/ipc_frame.h"
#include "src/management/service_core.h"
#include "src/service/public_command_endpoint.h"
#include "src/service/service_host.h"

#include <memory>
#include <mutex>
#include <string>

namespace myrd::ipc {
class PublicPipeServer;
}

namespace myrd::service {

class PublicPipeSurface final : public IPublicCommandSurface {
public:
    PublicPipeSurface(management::ServiceCore& core,
                      std::timed_mutex& execution_gate,
                      std::wstring pipe_name = L"") noexcept;
    ~PublicPipeSurface() noexcept override;

    PublicPipeSurface(const PublicPipeSurface&) = delete;
    PublicPipeSurface& operator=(const PublicPipeSurface&) = delete;

    [[nodiscard]] Result<void> open() override;
    [[nodiscard]] Result<void> close() override;

private:
    PublicCommandEndpoint endpoint_;
    std::wstring pipe_name_;
    std::unique_ptr<ipc::PublicPipeServer> server_;
};

} // namespace myrd::service
