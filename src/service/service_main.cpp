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
#include "src/product/product_version.h"
#include "src/service/service_host.h"
#include "src/service/service_runtime.h"
#include "src/service/service_system.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

namespace {

// Set by the console control handler and read by the foreground loop.  Atomic because the
// handler runs on a thread Windows owns: the shutdown itself must not run there, so the
// handler's only job is to make this flag visible.
std::atomic_bool console_stop_requested{false};

BOOL WINAPI console_control_handler(const DWORD control) {
    switch (control) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    case CTRL_LOGOFF_EVENT:
        console_stop_requested.store(true);
        return TRUE;
    default:
        return FALSE;
    }
}

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
               | SERVICE_ACCEPT_PRESHUTDOWN
               // Sleep and hibernate reach a service this way, and the checkpoint
               // policy has a rule for them (`on_suspend`).  Without this bit the
               // handler is never called and the rule has no consumer at all --
               // which is exactly the state this service was in before T1-08.
               | SERVICE_ACCEPT_POWEREVENT)
            : 0;
    status.dwCheckPoint =
        (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0
                                                              : status.dwCheckPoint + 1;
    SetServiceStatus(handle, &status);
}

// Records a host failure in the exit code the SCM will show an operator, and
// says which call failed.  In service mode there is no console, so the exit code
// is what survives -- and it is deliberately not `NO_ERROR`.
//
// **A service has no stderr, so the line also goes to the durable log.**  Measured
// (R-08): an installed service that refused an unsupported durable schema died with a
// non-zero exit code and no readable message anywhere -- `sc query` shows 1066 and that is
// all.  The same line is appended to `<ProgramData>\MyRamDisk\logs\service.log`, the file
// the installer made readable to Users, so an operator can read why it refused to start.
void log_startup_failure(const char* line) {
    std::fprintf(stderr, "%s", line);
    std::fflush(stderr);
    wchar_t program_data[MAX_PATH]{};
    const DWORD length =
        GetEnvironmentVariableW(L"ProgramData", program_data, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;
    std::wstring path(program_data);
    path += L"\\MyRamDisk\\logs\\service.log";
    const HANDLE file = CreateFileW(
        path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME now{};
    GetSystemTime(&now);
    char stamp[64]{};
    std::snprintf(stamp, sizeof(stamp), "%04u-%02u-%02uT%02u:%02u:%02uZ ",
                  now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute,
                  now.wSecond);
    DWORD written = 0;
    WriteFile(file, stamp, static_cast<DWORD>(strlen(stamp)), &written, nullptr);
    WriteFile(file, line, static_cast<DWORD>(strlen(line)), &written, nullptr);
    CloseHandle(file);
}

DWORD failure_exit_code(const char* what, const myrd::Error& error) {
    char line[1024]{};
    std::snprintf(line, sizeof(line),
                  "myrd-service: %s failed: op=%ls msg=%ls native=%u\n", what,
                  error.operation.c_str(), error.message.c_str(),
                  error.native_code);
    log_startup_failure(line);
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
    config.host.system_shutting_down = []() noexcept {
        return GetSystemMetrics(SM_SHUTTINGDOWN) != 0;
    };

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
DWORD WINAPI control_handler(const DWORD control, const DWORD event_type,
                             void* context, void*) {
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
    case SERVICE_CONTROL_POWEREVENT: {
        // **The machine is going to sleep, not shutting down.**
        //
        // PBT_APMSUSPEND is the only event acted on.  The others (resume, power
        // source changes) are acknowledged and otherwise ignored: a resume has no
        // work of its own here, and inventing some would be a second lifecycle this
        // service was never designed to run.
        //
        // A checkpoint is *not* performed on this thread.  The policy admits the work
        // durably and returns, so the answer to the SCM is bounded no matter how many
        // disks are running -- if the machine goes away before a commit lands, the
        // recovery path is what covers it.
        if (event_type == PBT_APMSUSPEND) {
            const auto handed = state->host->suspend();
            std::fprintf(stderr,
                         "myrd-service: suspend: handed %zu profile(s) a "
                         "best-effort checkpoint\n",
                         handed);
            std::fflush(stderr);
        }
        return NO_ERROR;
    }
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
//
// **It serves until interrupted, because a foreground service that starts and
// immediately stops cannot be diagnosed with.**  That shape was all this had, and it
// made the one thing an operator most needs -- run it in front of you and watch it --
// impossible: the only way to see a failure was to install the service, where the SCM
// gives the process no stderr at all.  Waiting here for a stop is also what makes the
// console mode usable as an end-to-end fixture: a script starts it, commands it over the
// public pipe, and the log it writes is the same log the installed service would have
// thrown away.
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
    std::fprintf(stderr,
                 "myrd-service: serving; press Ctrl+C to stop\n");
    std::fflush(stderr);

    // A console control event is the only stop signal a foreground process has that a
    // script can also send (CTRL_BREAK to the process group).  The handler only sets a
    // flag: the shutdown must run on this thread, through the same `host.stop()` the SCM
    // path uses, or the two stop paths would be two implementations.
    ::SetConsoleCtrlHandler(&console_control_handler, TRUE);
    while (!console_stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    ::SetConsoleCtrlHandler(&console_control_handler, FALSE);

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
        if (std::wcscmp(argv[index], L"--version") == 0) {
            // **One version, one constant (R-07).**  The CLI, the GUI and this binary all print
            // `myrd::product::kProductVersion`, so "the shipped pieces agree" is a property of the
            // build rather than a fact someone has to keep re-checking by hand.  The install test
            // compares the three outputs and the header they come from.
            std::printf("myrd-service %s\n", myrd::product::kProductVersion);
            std::fflush(stdout);
            return 0;
        }
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
