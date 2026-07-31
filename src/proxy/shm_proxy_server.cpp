#include "shm_proxy_server.h"

#include <chrono>

namespace myrd::proxy {

ShmProxyServer::ShmProxyServer(
    SharedMemoryChannel channel, RequestProcessor& processor)
    : channel_(std::move(channel)), processor_(processor) {
    stop_event_ = UniqueHandle(
        CreateEventW(nullptr, TRUE, FALSE, nullptr)); // manual-reset
}

ShmProxyServer::~ShmProxyServer() noexcept {
    stop();
}

Result<void> ShmProxyServer::start() {
    auto expected = ServerState::constructed;
    if (!state_.compare_exchange_strong(expected, ServerState::ready)) {
        return Result<void>::failure(
            {ErrorDomain::protocol, 0,
             L"ShmProxyServer::start",
             L"server already started or in wrong state"});
    }

    state_.store(ServerState::running);
    try {
        worker_thread_ = std::thread(&ShmProxyServer::worker_loop, this);
    } catch (...) {
        state_.store(ServerState::failed);
        return Result<void>::failure(
            {ErrorDomain::internal, 0,
             L"ShmProxyServer::start",
             L"failed to create worker thread"});
    }
    return Result<void>::success();
}

void ShmProxyServer::worker_loop() {
    HANDLE waits[2] = {channel_.request_event(), stop_event_.get()};

    for (;;) {
        DWORD rc = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

        if (rc == WAIT_OBJECT_0 + 1) {
            // Stop event
            return;
        }

        if (rc == WAIT_OBJECT_0) {
            auto outcome = processor_.process(channel_.mapping());
            switch (outcome.action) {
            case ProcessorAction::respond:
                SetEvent(channel_.response_event());
                break;
            case ProcessorAction::close:
                state_.store(ServerState::close_seen);
                return;
            case ProcessorAction::fatal:
                state_.store(ServerState::failed);
                worker_error_ = outcome.error;
                return;
            }
            continue;
        }

        // WAIT_FAILED or WAIT_ABANDONED
        if (rc == WAIT_FAILED) {
            state_.store(ServerState::failed);
            worker_error_ = Error{
                ErrorDomain::win32,
                static_cast<std::uint32_t>(GetLastError()),
                L"WaitForMultipleObjects",
                L"wait failed in worker loop"};
        }
        return;
    }
}

Result<void> ShmProxyServer::stop() {
    auto s = state_.load();

    // Already stopped or constructed: no-op
    if (s == ServerState::stopped || s == ServerState::constructed) {
        state_.store(ServerState::stopped);
        return Result<void>::success();
    }

    // Signal stop event
    SetEvent(stop_event_.get());

    // Join worker thread
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    state_.store(ServerState::stopped);
    return Result<void>::success();
}

ServerState ShmProxyServer::state() const noexcept {
    return state_.load();
}

std::optional<Error> ShmProxyServer::worker_error() const {
    return worker_error_;
}

} // namespace myrd::proxy
