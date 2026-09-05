#pragma once

#include "src/control/ipc_codec.h"
#include "src/control/pipe_transport.h"

#include <memory>

namespace myrd::ipc {

class PublicPipeClient final {
public:
    static Result<std::unique_ptr<PublicPipeClient>> connect(
        const OperationContext& context);

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
