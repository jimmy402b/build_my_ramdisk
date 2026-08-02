#pragma once

#include "src/proxy/request_processor.h"
#include "src/proxy/shared_memory_channel.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace myrd::proxy {

enum class SessionState : std::uint8_t {
    constructed, objects_created, ready, serving,
    closing, closed, failed
};

struct ShmProxySessionOptions {
    std::chrono::milliseconds request_timeout{30000};
    std::size_t max_recent_diagnostics = 64;
};

enum class SessionStartStage : std::uint8_t {
    cancel_monitor, request_worker, ready_publish
};

struct ShmProxySessionHooks {
    std::function<Result<void>(SessionStartStage)> before_start_stage;
    std::function<Result<void>(HANDLE, std::wstring_view)> signal_event;
};

struct SessionDiagnostic {
    std::uint64_t sequence;
    RequestMetadata request;
    Error error;
};

struct SessionSnapshot {
    SessionState state;
    RequestCounters requests;
    std::optional<Error> last_error;
    std::vector<SessionDiagnostic> recent_diagnostics;
};

class ShmProxySession final {
public:
    ShmProxySession(SharedMemoryChannel channel,
                    RequestProcessor& processor,
                    ShmProxySessionOptions options = {},
                    ShmProxySessionHooks hooks = {});
    ~ShmProxySession() noexcept;

    Result<void> start();
    Result<void> cancel();
    Result<void> join();
    Result<void> stop();
    SessionSnapshot snapshot() const;
    const ProxyObjectNames& names() const noexcept;

    ShmProxySession(const ShmProxySession&) = delete;
    ShmProxySession& operator=(const ShmProxySession&) = delete;

private:
    void run_request_worker(std::stop_token token);
    void run_cancel_monitor(std::stop_token token);

    SharedMemoryChannel channel_;
    RequestProcessor& processor_;
    ShmProxySessionOptions options_;
    ShmProxySessionHooks hooks_;

    mutable std::mutex mutex_;
    SessionState state_ = SessionState::constructed;
    std::optional<Error> last_error_;

    std::stop_source stop_source_;
    std::jthread request_thread_;
    std::jthread cancel_thread_;

    std::atomic<std::uint64_t> diagnostic_seq_{0};
    mutable std::mutex diag_mutex_;
    std::vector<SessionDiagnostic> diagnostics_;
};

} // namespace myrd::proxy
