#pragma once

#include "src/control/ipc_codec.h"
#include "src/control/pipe_transport.h"

#include <memory>

namespace myrd::ipc {

class PublicPipeClient final {
public:
    // **The missing half of the protocol: turning a command into a request frame.**
    //
    // The service side of the command surface is complete -- every command type is encoded by
    // `encode_command_payload`, every variant is decoded, the frame layer works, and `disk list` is already
    // handled with its read-only check and its authorization.  What did not exist anywhere was the mirror:
    // nothing built a sendable frame from a `control::Command`, which is why no CLI could exist and why this
    // class was compiled but unused.
    //
    // It lives here rather than in a new translation unit because this class already owns both halves of the
    // protocol -- the codec and the frame -- and because a request builder with no client to build for would
    // be one more thing to keep in step.
    //
    // `caller` is deliberately absent.  The service derives the caller from the pipe connection itself, so a
    // client cannot claim an identity by putting one in a frame; that is a property worth keeping visible,
    // which is why the parameter is missing rather than merely unset.
    static Result<Frame> build_command_request(const control::Command& command,
                                               const product::Uuid& request_id,
                                               bool no_wait = false,
                                               bool dry_run = false);

    // `pipe_name` mirrors the transport's and the server's, so a test can run a
    // real client against a real server without competing for the production
    // pipe.  Production always takes the default.
    static Result<std::unique_ptr<PublicPipeClient>> connect(
        const OperationContext& context,
        std::wstring_view pipe_name = kPublicControlPipeName);

    Result<Frame> request(const OperationContext& context,
                          const Frame& request);
    Result<Frame> wait_operation(const OperationContext& context,
                                 const Frame& request);

private:
    explicit PublicPipeClient(std::unique_ptr<PipeTransport> transport) noexcept;
    Result<Frame> request_once(const OperationContext& context,
                               const Frame& request);

    std::unique_ptr<PipeTransport> transport_;
    InFlightGate gate_;
};

} // namespace myrd::ipc
