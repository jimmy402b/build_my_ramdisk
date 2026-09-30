#pragma once

#include "src/control/authorization.h"
#include "src/control/pipe_transport.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace myrd::ipc {

using PublicPipeHandler = std::function<Result<Frame>(
    const OperationContext&, const CallerIdentity&, const Frame&)>;

class PublicPipeServer final {
public:
    // `pipe_name` exists for the same reason the client's connect takes one: so
    // a test can run a real server and a real client over the real transport
    // without competing for the production pipe, which an installed service may
    // already own.  Production always takes the default.
    static Result<std::unique_ptr<PublicPipeServer>> start(
        PublicPipeHandler handler,
        std::wstring_view pipe_name = kPublicControlPipeName);

    ~PublicPipeServer() noexcept;
    PublicPipeServer(const PublicPipeServer&) = delete;
    PublicPipeServer& operator=(const PublicPipeServer&) = delete;

    Result<void> stop(const OperationContext& context);

private:
    PublicPipeServer(PublicPipeHandler handler,
                     std::wstring pipe_name,
                     void* first_instance);
    void run(std::stop_token stop_token, void* first_instance) noexcept;

    PublicPipeHandler handler_;
    std::wstring pipe_name_;
    std::vector<std::jthread> workers_;
    std::mutex state_mutex_;
    std::condition_variable state_cv_;
    std::size_t workers_done_{};
};

} // namespace myrd::ipc
