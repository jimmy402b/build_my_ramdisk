#include "shm_proxy_session.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <future>

namespace myrd::proxy {

ShmProxySession::ShmProxySession(SharedMemoryChannel channel,
                                 RequestProcessor& processor,
                                 ShmProxySessionOptions options,
                                 ShmProxySessionHooks hooks)
    : channel_(std::move(channel)), processor_(processor),
      options_(options), hooks_(std::move(hooks)) {}

ShmProxySession::~ShmProxySession() noexcept {
    stop();
}

const ProxyObjectNames& ShmProxySession::names() const noexcept {
    return channel_.names();
}

Result<void> ShmProxySession::start() {
    std::scoped_lock lock(mutex_);
    if (state_ != SessionState::constructed) {
        return Result<void>::failure({ErrorDomain::protocol, 0,
            L"ShmProxySession::start", L"session already started"});
    }
    state_ = SessionState::objects_created;

    // Launch cancel monitor thread
    if (hooks_.before_start_stage) {
        auto r = hooks_.before_start_stage(SessionStartStage::cancel_monitor);
        if (!r) {
            state_ = SessionState::failed;
            return r;
        }
    }
    std::promise<Result<void>> ready_promise;
    auto ready_future = ready_promise.get_future();

    cancel_thread_ = std::jthread(
        [this](std::stop_token token) { run_cancel_monitor(token); });

    if (hooks_.before_start_stage) {
        auto r = hooks_.before_start_stage(SessionStartStage::request_worker);
        if (!r) {
            state_ = SessionState::failed;
            stop_source_.request_stop();
            SetEvent(channel_.cancel_event());
            if (cancel_thread_.joinable()) cancel_thread_.join();
            return r;
        }
    }
    request_thread_ = std::jthread(
        [this, promise = std::move(ready_promise),
         token = stop_source_.get_token()]() mutable {
        try {
            // Publish ready
            {
                std::scoped_lock lk(mutex_);
                state_ = SessionState::ready;
            }
            promise.set_value(Result<void>::success());

            // Transition to serving
            {
                std::scoped_lock lk(mutex_);
                state_ = SessionState::serving;
            }

            // Main request loop
            while (!token.stop_requested()) {
                HANDLE handles[] = {
                    channel_.request_event(),
                    channel_.cancel_event()
                };
                DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
                if (wait == WAIT_OBJECT_0 + 1 || token.stop_requested()) {
                    break; // Cancel signaled or stop requested
                }
                if (wait != WAIT_OBJECT_0) {
                    std::scoped_lock lk(mutex_);
                    last_error_ = Error{ErrorDomain::win32,
                        static_cast<std::uint32_t>(GetLastError()),
                        L"WaitForMultipleObjects", L"wait failed"};
                    state_ = SessionState::failed;
                    break;
                }

                // Process request
                OperationContext ctx{
                    token,
                    std::chrono::steady_clock::now() + options_.request_timeout
                };
                auto outcome = processor_.process(ctx, channel_.mapping());

                {
                    std::scoped_lock lk(mutex_);
                    if (state_ != SessionState::serving) break;
                }

                if (outcome.action == ProcessorAction::respond) {
                    if (hooks_.signal_event) {
                        auto r = hooks_.signal_event(
                            channel_.response_event(), L"Response");
                        if (!r) {
                            std::scoped_lock lk(mutex_);
                            last_error_ = r.error();
                            state_ = SessionState::failed;
                            break;
                        }
                    } else {
                        SetEvent(channel_.response_event());
                    }
                    if (outcome.error.has_value()) {
                        SessionDiagnostic diag{
                            diagnostic_seq_++, outcome.request,
                            outcome.error.value()};
                        std::scoped_lock dlk(diag_mutex_);
                        diagnostics_.push_back(diag);
                        if (diagnostics_.size() > options_.max_recent_diagnostics) {
                            diagnostics_.erase(diagnostics_.begin());
                        }
                    }
                } else if (outcome.action == ProcessorAction::close) {
                    std::scoped_lock lk(mutex_);
                    state_ = SessionState::closing;
                    break;
                } else if (outcome.action == ProcessorAction::fatal) {
                    std::scoped_lock lk(mutex_);
                    last_error_ = outcome.error;
                    state_ = SessionState::failed;
                    if (outcome.error.has_value()) {
                        SessionDiagnostic diag{
                            diagnostic_seq_++, outcome.request,
                            outcome.error.value()};
                        std::scoped_lock dlk(diag_mutex_);
                        diagnostics_.push_back(diag);
                    }
                    break;
                }
            }
        } catch (...) {
            std::scoped_lock lk(mutex_);
            last_error_ = Error{ErrorDomain::internal,
                static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
                L"request_worker", L"unhandled exception"};
            state_ = SessionState::failed;
            promise.set_value(Result<void>::failure(last_error_.value()));
        }
    });

    auto ready_result = ready_future.get();
    if (!ready_result.ok()) {
        return ready_result;
    }
    return Result<void>::success();
}

void ShmProxySession::run_cancel_monitor(std::stop_token token) {
    while (!token.stop_requested()) {
        DWORD wait = WaitForSingleObject(channel_.cancel_event(), INFINITE);
        if (wait == WAIT_OBJECT_0) {
            stop_source_.request_stop();
            break;
        }
    }
}

Result<void> ShmProxySession::cancel() {
    stop_source_.request_stop();
    SetEvent(channel_.cancel_event());
    return Result<void>::success();
}

Result<void> ShmProxySession::join() {
    if (cancel_thread_.joinable()) cancel_thread_.join();
    if (request_thread_.joinable()) request_thread_.join();
    std::scoped_lock lk(mutex_);
    if (state_ != SessionState::failed) {
        state_ = SessionState::closed;
    }
    return Result<void>::success();
}

Result<void> ShmProxySession::stop() {
    cancel();
    return join();
}

SessionSnapshot ShmProxySession::snapshot() const {
    SessionSnapshot snap;
    {
        std::scoped_lock lk(mutex_);
        snap.state = state_;
        snap.last_error = last_error_;
    }
    snap.requests = processor_.counters();
    {
        std::scoped_lock lk(diag_mutex_);
        snap.recent_diagnostics = diagnostics_;
    }
    return snap;
}

} // namespace myrd::proxy
