#pragma once

#include "src/control/authorization.h"
#include "src/control/pipe_transport.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <thread>
#include <vector>

namespace myrd::ipc {

using PublicPipeHandler = std::function<Result<Frame>(
    const OperationContext&, const CallerIdentity&, const Frame&)>;

class PublicPipeServer final {
public:
    static Result<std::unique_ptr<PublicPipeServer>> start(
        PublicPipeHandler handler);

    ~PublicPipeServer() noexcept;
    PublicPipeServer(const PublicPipeServer&) = delete;
    PublicPipeServer& operator=(const PublicPipeServer&) = delete;

    Result<void> stop(const OperationContext& context);

private:
    PublicPipeServer(PublicPipeHandler handler, void* first_instance);
    void run(std::stop_token stop_token, void* first_instance) noexcept;

    PublicPipeHandler handler_;
    std::vector<std::jthread> workers_;
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    std::size_t workers_done_{};
};

} // namespace myrd::ipc
