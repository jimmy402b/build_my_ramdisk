// myrd-service: the Windows service entry point.
//
// Everything below this file is ordinary testable code; this file is the part
// that can only be exercised by a service control manager, and it is kept as
// thin as that allows.  It does three things and no more: tell the SCM what this
// service accepts, translate SCM control codes into `ServiceHost` calls, and keep
// the SCM's view of the state in step with the host's.  Any decision that is not
// one of those three belongs in `ServiceHost`, where it can be tested.
//
// It can also be run interactively (`myrd-service.exe --console`), which is how
// the start and clean-exit paths are exercised without installing anything.  That
// mode is not a second implementation: it drives the same host through the same
// calls, so a service that works in one mode works in the other.

#include "src/common/result.h"
#include "src/service/service_host.h"
#include "src/service/service_runtime.h"
#include "src/service/service_system.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace {

using myrd::Result;
using myrd::service::ServiceHost;
using myrd::service::ServiceHostState;

constexpr wchar_t kServiceName[] = L"MyRamDiskService";

// The stop budget the SCM is asked to allow.  Kept in one place because the
// service's own budget has to fit inside it: a host allowed longer than the SCM
// will wait is a host the SCM kills mid-shutdown.
constexpr DWORD kStopBudgetMs = 120'000;

// The whole process state the control handler needs, as one object.
//
// A control handler is a free function reached by the SCM through a function
// pointer, so it has no `this`.  The state pointer the SCM carries is how the
// handler finds the host -- and routing it through one struct means the handler
// never has to know how the service was composed.
struct ServiceState final {
    SERVICE_STATUS_HANDLE status_handle{};
    SERVICE_STATUS status{};
    // The host is heap-owned and lives for the whole service lifetime, because
    // the handler is called from another thread and must not be handed a pointer
    // to a local that a different code path may have left.
    std::unique_ptr<ServiceHost> host;
    // The runtime owns the stores, the worker control and the scheduler that the
    // host borrows; it must outlive the host, which is why it sits beside it here
    // and is destroyed after it.
    std::unique_ptr<myrd::service::ServiceRuntime> runtime;
};

// The process's one service state.
//
// `ServiceMain` is reached through a function pointer and the control handler
// through another, so neither can be a member function and neither can take the
// state as an argument of its own choosing.  The handler does receive an opaque
// context pointer, which is where the state goes for *it*; `ServiceMain` has no
// such parameter, so it reads this.  A file-static is honest about what this is
// -- one process, one service -- rather than dressing it up.
//
// It is a raw pointer rather than a smart one because the SCM calls `ServiceMain`
// and expects it to return; ownership stays in `run_service`, which outlives the
// dispatcher call.
ServiceState* g_state = nullptr;

void report(SERVICE_STATUS_HANDLE handle, SERVICE_STATUS& status,
            const DWORD state, const DWORD win32_exit = NO_ERROR,
            const DWORD wait_hint = 0) {
    status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState = state;
    status.dwWin32ExitCode = win32_exit;
    status.dwWaitHint = wait_hint;
    // Only a running service reports "accepts this control"; a pending one
    // accepts nothing, which is what stops a second stop arriving mid-shutdown.
    status.dwControlsAccepted =
        state == SERVICE_RUNNING
            ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN
               | SERVICE_ACCEPT_PRESHUTDOWN)
            : 0;
    status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0
                                                              : status.dwCheckPoint + 1;
    SetServiceStatus(handle, &status);
}

// Records a host failure in the exit code the SCM will show an operator, and
// says which call failed.  In service mode there is no console, so the exit code
// is what survives -- and it is deliberately not `NO_ERROR`.
DWORD failure_exit_code(const char* what, const myrd::Error& error) {
    std::fprintf(stderr, "myrd-service: %s failed: op=%ls msg=%ls native=%u\n",
                 what, error.operation.c_str(), error.message.c_str(),
                 error.native_code);
    std::fflush(stderr);
    return ERROR_SERVICE_SPECIFIC_ERROR;
}

DWORD failure_exit_code(const char* what, const Result<void>& result) {
    return failure_exit_code(what, result.error());
}

// Builds the runtime and the host the SCM will drive.
//
// `argv[0]` is the path this executable was started from, and the worker sits
// beside it: the worker's location is an installation question, and deriving it
// from our own module is how the two stay in step through a reinstall.
Result<std::unique_ptr<ServiceState>> build_state() {
    wchar_t module[MAX_PATH]{};
    const DWORD written =
        GetModuleFileNameW(nullptr, module, static_cast<DWORD>(MAX_PATH));
    if (written == 0 || written >= MAX_PATH) {
        return Result<std::unique_ptr<ServiceState>>::failure(
            {myrd::ErrorDomain::win32, GetLastError(), L"GetModuleFileNameW",
             L"the service's own path could not be resolved", GetLastError()});
    }
    std::wstring own_directory(module);
    const auto separator = own_directory.find_last_of(L'\\');
    if (separator == std::wstring::npos) {
        return Result<std::unique_ptr<ServiceState>>::failure(
            {myrd::ErrorDomain::usage, 1, L"GetModuleFileNameW",
             L"the service's own path has no directory", 0});
    }
    own_directory.resize(separator);
    const std::wstring worker = own_directory + L"\\myrd-m5-worker.exe";

    // `%ProgramData%` rather than a literal path: the installer decides where the
    // durable roots live, and a service that hard-coded a location would silently
    // stop finding them the moment that decision changed.
    wchar_t program_data[MAX_PATH]{};
    const DWORD program_data_length = GetEnvironmentVariableW(
        L"ProgramData", program_data, static_cast<DWORD>(MAX_PATH));
    if (program_data_length == 0 || program_data_length >= MAX_PATH) {
        return Result<std::unique_ptr<ServiceState>>::failure(
            {myrd::ErrorDomain::win32, GetLastError(), L"GetEnvironmentVariableW",
             L"ProgramData could not be read", GetLastError()});
    }

    myrd::service::ServiceRuntimeConfig config{};
    config.program_data_root = program_data;
    config.worker_executable = worker;
    config.host.stop_budget_ms = kStopBudgetMs;

    auto runtime = myrd::service::ServiceRuntime::create(std::move(config));
    if (!runtime.ok()) {
        return Result<std::unique_ptr<ServiceState>>::failure(runtime.error());
    }
    auto state = std::make_unique<ServiceState>();
    state->runtime = runtime.move_value();
    state->host = std::make_unique<ServiceHost>(state->runtime->host());
    return Result<std::unique_ptr<ServiceState>>::success(std::move(state));
}

// The SCM's control handler.  One translation per control code and nothing else:
// every decision lives in `ServiceHost`, which is the point of keeping this thin.
DWORD WINAPI control_handler(const DWORD control, const DWORD, void* context,
                             void*) {
    // The state comes from the file-static, not from the context pointer this
    // handler was registered with -- and that is a measured correction, not a
    // preference.  Windows called this handler with `context == nullptr` even
    // though a valid context was passed to `RegisterServiceCtrlHandlerExW`, so a
    // handler that trusted the context answered every control with a failure and
    // the service could not be stopped.  `sc stop` reported no error at all while
    // the service stayed RUNNING, which is exactly why the failure was invisible.
    //
    // The parameter is still accepted and deliberately unused: it is part of the
    // handler's required signature, and naming it `context` keeps the reason for
    // ignoring it readable.
    (void)context;
    auto* state = g_state;
    if (state == nullptr) return ERROR_SERVICE_SPECIFIC_ERROR;

    switch (control) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    case SERVICE_CONTROL_PRESHUTDOWN: {
        // Reported before the work starts, because that is what the status is
        // for: the SCM has to know the service is on its way down or it will
        // apply its own timeout to a shutdown that has legitimately begun.  The
        // wait hint is the budget the host was configured with, so the SCM's
        // patience and the host's budget are the same number.
        report(state->status_handle, state->status, SERVICE_STOP_PENDING,
               NO_ERROR, kStopBudgetMs);
        const auto stopped = control == SERVICE_CONTROL_PRESHUTDOWN
            ? state->host->preshutdown()
            : state->host->stop();
        const DWORD exit_code =
            stopped.ok() ? NO_ERROR : failure_exit_code("stop", stopped);
        report(state->status_handle, state->status, SERVICE_STOPPED, exit_code);
        return NO_ERROR;
    }
    case SERVICE_CONTROL_INTERROGATE:
        // The SCM asking what state we are in.  The host is the authority; the
        // cached status is only the last thing reported, so it is refreshed from
        // the host rather than echoed.
        report(state->status_handle, state->status,
               state->host->running() ? SERVICE_RUNNING : SERVICE_STOPPED);
        return NO_ERROR;
    default:
        return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

// `ServiceMain`: the SCM's entry point into this process.
void WINAPI service_main(const DWORD, LPWSTR*) {
    ServiceState* const state = g_state;
    if (state == nullptr) return;

    // Registered with the *state* as its context, not with any member of it.
    // The handler is handed this pointer on every control and casts it back to
    // `ServiceState`; a pointer to one member would be read as a `ServiceState`
    // and every field after that member would be nonsense.
    state->status_handle = RegisterServiceCtrlHandlerExW(
        kServiceName, control_handler, state);
    if (state->status_handle == nullptr) return;

    state->status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    report(state->status_handle, state->status, SERVICE_START_PENDING, NO_ERROR,
           kStopBudgetMs);

    const auto started = state->host->start();
    if (!started.ok()) {
        // A start that failed reports STOPPED with a non-zero code rather than
        // staying pending: the SCM can act on a stopped service, and an operator
        // reading `sc query` gets a definite answer instead of a hang.
        report(state->status_handle, state->status, SERVICE_STOPPED,
               failure_exit_code("start", started));
        return;
    }
    report(state->status_handle, state->status, SERVICE_RUNNING);

    // The SCM owns this process's lifetime, so there is nothing to wait for: the
    // dispatcher returns once the service is stopped, and the process exits.  A
    // sleep loop here would be a second, worse copy of the SCM's own bookkeeping.
}

// Interactive mode.  Drives the same host through the same calls, so this is a
// way to exercise start and clean exit without installing anything -- not a
// second implementation.
int run_console(myrd::service::ServiceRuntime& runtime) {
    auto host = runtime.host();
    std::fprintf(stderr, "myrd-service: console mode, state=%s\n",
                 myrd::service::to_string(host.state()));
    const auto started = host.start();
    if (!started.ok()) {
        failure_exit_code("start", started);
        return 1;
    }
    std::fprintf(stderr, "myrd-service: started, state=%s recovered=%zu\n",
                 myrd::service::to_string(host.state()),
                 host.diagnostics().recovered_operations);
    std::fflush(stderr);
    const auto stopped = host.stop();
    if (!stopped.ok()) {
        failure_exit_code("stop", stopped);
        return 1;
    }
    std::fprintf(stderr, "myrd-service: stopped, state=%s\n",
                 myrd::service::to_string(host.state()));
    std::fflush(stderr);
    return 0;
}

int run_service() {
    auto state = build_state();
    if (!state.ok()) {
        // Nothing can be reported to the SCM yet -- there is no status handle
        // until `RegisterServiceCtrlHandlerExW`, which `ServiceMain` does.  So
        // the failure is named here and the SCM learns it from the process exit
        // code, which is the only channel that exists at this point.
        failure_exit_code("build_state", state.error());
        return 1;
    }
    std::unique_ptr<ServiceState> owned = state.move_value();
    g_state = owned.get();

    SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), service_main},
        {nullptr, nullptr}};
    if (StartServiceCtrlDispatcherW(table) == FALSE) {
        const DWORD error = GetLastError();
        std::fprintf(stderr,
                     "myrd-service: StartServiceCtrlDispatcher failed err=%lu\n",
                     static_cast<unsigned long>(error));
        std::fflush(stderr);
        return 1;
    }
    g_state = nullptr;
    return 0;
}

int run_console_entry() {
    auto state = build_state();
    if (!state.ok()) {
        failure_exit_code("build_state", state.error());
        return 1;
    }
    return run_console(*state.value()->runtime);
}

} // namespace

int wmain(const int argc, wchar_t** argv) {
    for (int index = 1; index < argc; ++index) {
        if (std::wcscmp(argv[index], L"--console") == 0) {
            return run_console_entry();
        }
        if (std::wcscmp(argv[index], L"--name") == 0) {
            // A dispatcher is normally invoked with the service name as its first
            // argument.  The name is fixed here rather than taken from the
            // command line, so this is accepted and ignored: a service that
            // answered to whatever name it was called with could be registered
            // twice under two names and be two services sharing one store.
            continue;
        }
        std::fprintf(stderr,
                     "myrd-service: unknown argument; use --console to run in "
                     "the foreground\n");
        return 2;
    }
    return run_service();
}
