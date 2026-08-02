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
    {
        std::scoped_lock lock(mutex_);
        if (state_ != SessionState::constructed) {
            return Result<void>::failure({ErrorDomain::protocol, 0,
                L"ShmProxySession::start", L"session already started"});
        }
        state_ = SessionState::objects_created;
    }

    const auto fail_start = [this](const Error& error) {
        std::scoped_lock lock(mutex_);
        last_error_ = error;
        state_ = SessionState::failed;
    };
    const auto stop_started_threads = [this] {
        stop_source_.request_stop();
        request_thread_.request_stop();
        cancel_thread_.request_stop();
        SetEvent(channel_.cancel_event());
        if (request_thread_.joinable()) request_thread_.join();
        if (cancel_thread_.joinable()) cancel_thread_.join();
    };

    try {
        if (hooks_.before_start_stage) {
            auto result = hooks_.before_start_stage(
                SessionStartStage::cancel_monitor);
            if (!result) {
                fail_start(result.error());
                return result;
            }
        }

        cancel_thread_ = std::jthread(
            [this](std::stop_token token) { run_cancel_monitor(token); });

        if (hooks_.before_start_stage) {
            auto result = hooks_.before_start_stage(
                SessionStartStage::request_worker);
            if (!result) {
                fail_start(result.error());
                stop_started_threads();
                return result;
            }
        }

        std::promise<Result<void>> ready_promise;
        auto ready_future = ready_promise.get_future();
        request_thread_ = std::jthread(
            [this, promise = std::move(ready_promise),
             token = stop_source_.get_token()]() mutable {
            bool ready_published = false;
            try {
                if (hooks_.before_start_stage) {
                    auto result = hooks_.before_start_stage(
                        SessionStartStage::ready_publish);
                    if (!result) {
                        {
                            std::scoped_lock lock(mutex_);
                            last_error_ = result.error();
                            state_ = SessionState::failed;
                        }
                        promise.set_value(Result<void>::failure(result.error()));
                        ready_published = true;
                        stop_source_.request_stop();
                        SetEvent(channel_.cancel_event());
                        return;
                    }
                }

                {
                    std::scoped_lock lock(mutex_);
                    state_ = SessionState::ready;
                }
                promise.set_value(Result<void>::success());
                ready_published = true;

                {
                    std::scoped_lock lock(mutex_);
                    if (state_ == SessionState::ready) {
                        state_ = SessionState::serving;
                    }
                }

                while (!token.stop_requested()) {
                    HANDLE handles[] = {
                        channel_.request_event(),
                        channel_.cancel_event()
                    };
                    const DWORD wait = WaitForMultipleObjects(
                        2, handles, FALSE, INFINITE);
                    if (wait == WAIT_OBJECT_0 + 1 || token.stop_requested()) {
                        break;
                    }
                    if (wait != WAIT_OBJECT_0) {
                        std::scoped_lock lock(mutex_);
                        last_error_ = Error{
                            ErrorDomain::win32,
                            static_cast<std::uint32_t>(GetLastError()),
                            L"WaitForMultipleObjects", L"wait failed"};
                        state_ = SessionState::failed;
                        break;
                    }

                    OperationContext context{
                        token,
                        std::chrono::steady_clock::now() +
                            options_.request_timeout
                    };
                    auto outcome = processor_.process(
                        context, channel_.mapping());

                    {
                        std::scoped_lock lock(mutex_);
                        if (state_ != SessionState::serving) break;
                    }

                    if (outcome.action == ProcessorAction::respond) {
                        Result<void> signal = Result<void>::success();
                        if (hooks_.signal_event) {
                            signal = hooks_.signal_event(
                                channel_.response_event(), L"Response");
                        } else if (!SetEvent(channel_.response_event())) {
                            signal = Result<void>::failure({
                                ErrorDomain::win32,
                                static_cast<std::uint32_t>(GetLastError()),
                                L"SetEvent(response)",
                                L"failed to signal response event"});
                        }
                        if (!signal) {
                            std::scoped_lock lock(mutex_);
                            last_error_ = signal.error();
                            state_ = SessionState::failed;
                            break;
                        }
                        if (outcome.error.has_value()) {
                            record_diagnostic(outcome);
                        }
                    } else if (outcome.action == ProcessorAction::close) {
                        std::scoped_lock lock(mutex_);
                        state_ = SessionState::closing;
                        break;
                    } else {
                        {
                            std::scoped_lock lock(mutex_);
                            last_error_ = outcome.error;
                            state_ = SessionState::failed;
                        }
                        if (outcome.error.has_value()) {
                            record_diagnostic(outcome);
                        }
                        break;
                    }
                }
            } catch (...) {
                const Error error{
                    ErrorDomain::internal,
                    static_cast<std::uint32_t>(
                        OperationErrorCode::internal_invariant),
                    L"request_worker", L"unhandled exception"};
                {
                    std::scoped_lock lock(mutex_);
                    last_error_ = error;
                    state_ = SessionState::failed;
                }
                if (!ready_published) {
                    promise.set_value(Result<void>::failure(error));
                }
            }
            stop_source_.request_stop();
            SetEvent(channel_.cancel_event());
        });

        auto ready_result = ready_future.get();
        if (!ready_result) {
            stop_started_threads();
            return ready_result;
        }
        return Result<void>::success();
    } catch (...) {
        const Error error{
            ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            L"ShmProxySession::start", L"failed to start worker threads"};
        fail_start(error);
        stop_started_threads();
        return Result<void>::failure(error);
    }
}

void ShmProxySession::record_diagnostic(const ProcessOutcome& outcome) {
    if (!outcome.error.has_value() || options_.max_recent_diagnostics == 0) {
        return;
    }
    SessionDiagnostic diagnostic{
        diagnostic_seq_++, outcome.request, outcome.error.value()};
    std::scoped_lock lock(diag_mutex_);
    diagnostics_.push_back(std::move(diagnostic));
    if (diagnostics_.size() > options_.max_recent_diagnostics) {
        diagnostics_.erase(
            diagnostics_.begin(),
            diagnostics_.begin() + static_cast<std::ptrdiff_t>(
                diagnostics_.size() - options_.max_recent_diagnostics));
    }
}

void ShmProxySession::run_cancel_monitor(std::stop_token token) {
    while (!token.stop_requested()) {
        const DWORD wait = WaitForSingleObject(channel_.cancel_event(), 100);
        if (wait == WAIT_OBJECT_0) {
            stop_source_.request_stop();
            break;
        }
        if (wait == WAIT_FAILED) {
            std::scoped_lock lock(mutex_);
            last_error_ = Error{
                ErrorDomain::win32,
                static_cast<std::uint32_t>(GetLastError()),
                L"WaitForSingleObject(cancel)", L"wait failed"};
            state_ = SessionState::failed;
            stop_source_.request_stop();
            break;
        }
    }
}

Result<void> ShmProxySession::cancel() {
    stop_source_.request_stop();
    request_thread_.request_stop();
    cancel_thread_.request_stop();
    if (!SetEvent(channel_.cancel_event())) {
        return Result<void>::failure({
            ErrorDomain::win32,
            static_cast<std::uint32_t>(GetLastError()),
            L"SetEvent(cancel)", L"failed to signal cancel event"});
    }
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
    auto cancelled = cancel();
    auto joined = join();
    if (!cancelled) return cancelled;
    return joined;
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
