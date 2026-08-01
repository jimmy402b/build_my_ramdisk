#pragma once

#include "request_processor.h"
#include "shared_memory_channel.h"
#include "src/common/result.h"
#include "src/common/unique_handle.h"

#include <atomic>
#include <optional>
#include <thread>

namespace myrd::proxy {

enum class ServerState : std::uint8_t {
    constructed, ready, running, close_seen, stopped, failed
};

class ShmProxyServer final {
public:
    ShmProxyServer(SharedMemoryChannel channel, RequestProcessor& processor);
    ~ShmProxyServer() noexcept;

    Result<void> start();
    Result<void> stop();
    ServerState state() const noexcept;
    std::optional<Error> worker_error() const;

private:
    void worker_loop();

    SharedMemoryChannel channel_;
    RequestProcessor& processor_;
    UniqueHandle stop_event_;       // manual-reset
    std::thread worker_thread_;
    std::atomic<ServerState> state_{ServerState::constructed};
    std::optional<Error> worker_error_;
};

} // namespace myrd::proxy
