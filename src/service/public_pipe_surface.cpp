#include "src/service/public_pipe_surface.h"

#include "src/common/operation_context.h"
#include "src/control/public_pipe_server.h"

#include <chrono>
#include <utility>

namespace myrd::service {

namespace {

// The budget one accepted request may take, end to end.  It has to be longer than a
// client's own patience, because the client is what should give up first: a server
// that abandoned a request the client was still waiting for would leave the client
// unable to tell a refusal from a failure.
constexpr auto kRequestBound = std::chrono::minutes(5);

constexpr auto kStopBound = std::chrono::seconds(30);

} // namespace

PublicPipeSurface::PublicPipeSurface(management::ServiceCore& core,
                                     std::timed_mutex& execution_gate,
                                     std::wstring pipe_name) noexcept
    : endpoint_(core, execution_gate), pipe_name_(std::move(pipe_name)) {}

PublicPipeSurface::~PublicPipeSurface() noexcept { static_cast<void>(close()); }

Result<void> PublicPipeSurface::open() {
    if (server_ != nullptr) return Result<void>::success();
    auto started = ipc::PublicPipeServer::start(
        [this](const OperationContext& context,
               const ipc::CallerIdentity& caller,
               const ipc::Frame& request) {
            return endpoint_.handle(context, caller, request);
        },
        pipe_name_.empty()
            ? std::wstring_view(ipc::kPublicControlPipeName)
            : std::wstring_view(pipe_name_));
    if (!started.ok()) {
        return Result<void>::failure(started.error());
    }
    server_ = started.move_value();
    return Result<void>::success();
}

Result<void> PublicPipeSurface::close() {
    if (server_ == nullptr) return Result<void>::success();
    const OperationContext context{
        std::stop_token{},
        std::chrono::steady_clock::now() + kStopBound};
    auto stopped = server_->stop(context);
    // Released whatever the stop reported: a server whose workers did not retire
    // cleanly must not be restarted on the next `open`, because the second instance
    // would share the pipe with the first.
    server_.reset();
    return stopped;
}

} // namespace myrd::service
