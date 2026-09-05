#pragma once

#include "src/common/result.h"
#include "src/host/host_ipc.h"
#include "src/management/worker_supervisor.h"
#include "src/product/uuid.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::management::windows {

inline constexpr std::wstring_view kServiceName = L"MyRamDiskService";
inline constexpr std::wstring_view kServiceDisplayName = L"MyRamDisk Service";
inline constexpr std::wstring_view kServiceSingletonName =
    L"Global\\MyRamDisk.Service.Singleton.v1";
inline constexpr std::wstring_view kWorkerExecutableName =
    L"MyRamDiskHost.exe";
inline constexpr std::uint32_t kPreshutdownTimeoutMs = 120'000;
inline constexpr std::uint32_t kSuspendTimeoutMs = 10'000;

enum class WindowsServiceError : std::uint32_t {
    invalid_argument = 0x5B01,
    invalid_service_name,
    scm_open_failed,
    scm_configuration_failed,
    dispatcher_failed,
    control_handler_failed,
    singleton_exists,
    startup_failed,
    unsafe_worker_path,
    worker_launch_failed,
    worker_identity_failed,
    worker_startup_failed,
    worker_wait_failed,
};

struct ScmContract final {
    std::uint32_t service_type{};
    std::uint32_t start_type{};
    bool delayed_auto_start{};
    bool unrestricted_service_sid{};
    std::uint32_t failure_reset_seconds{};
    std::vector<std::uint32_t> restart_delays_ms;
    std::uint32_t preshutdown_timeout_ms{};

    bool operator==(const ScmContract&) const noexcept = default;
};

ScmContract scm_contract();

enum class StartupStage : std::uint8_t {
    logging = 0,
    singleton,
    worker_identity,
    driver,
    profiles,
    reattach,
    auto_start,
};

struct StartupProfile final {
    product::Uuid profile_id;
    bool auto_start{};
};

struct StartupProfileFailure final {
    product::Uuid profile_id;
    StartupStage stage{StartupStage::profiles};
    Error error;
};

struct StartupReport final {
    bool globally_ready{};
    std::vector<StartupStage> completed_stages;
    std::vector<product::Uuid> attached_profiles;
    std::vector<product::Uuid> started_profiles;
    std::vector<StartupProfileFailure> profile_failures;
};

class IServiceStartupActions {
public:
    virtual ~IServiceStartupActions() noexcept = default;
    virtual Result<void> initialize_logging() = 0;
    virtual Result<void> acquire_singleton() = 0;
    virtual Result<void> verify_worker_identity() = 0;
    virtual Result<void> verify_driver() = 0;
    virtual Result<std::vector<StartupProfile>> discover_profiles() = 0;
    virtual Result<void> validate_profile(
        const product::Uuid& profile_id) = 0;
    // true means an authenticated worker was attached. false means no worker
    // existed and a healthy auto-start profile may be launched serially.
    virtual Result<bool> reattach_profile(
        const product::Uuid& profile_id) = 0;
    virtual Result<void> start_profile(
        const product::Uuid& profile_id) = 0;
};

Result<StartupReport> run_service_startup(IServiceStartupActions& actions);

enum class ServiceSteadyState : std::uint8_t {
    running = 0,
    stopped,
};

struct BestEffortCheckpointResult final {
    std::vector<product::Uuid> attempted_profiles;
    std::vector<product::Uuid> confirmed_profiles;
    std::vector<product::Uuid> unresolved_profiles;
    bool deadline_exhausted{};
};

struct ControlledStopResult final {
    std::vector<product::Uuid> attempted_profiles;
    std::vector<product::Uuid> safely_stopped_profiles;
    std::vector<product::Uuid> unsafe_profiles;
    ServiceSteadyState resulting_state{ServiceSteadyState::running};
};

class IServiceShutdownActions {
public:
    virtual ~IServiceShutdownActions() noexcept = default;
    virtual std::uint64_t monotonic_time_ms() const noexcept = 0;
    virtual Result<std::vector<product::Uuid>> running_profiles() = 0;
    virtual Result<bool> checkpoint_best_effort(
        const product::Uuid& profile_id,
        std::uint64_t monotonic_deadline_ms) = 0;
    virtual Result<bool> stop_safely(
        const product::Uuid& profile_id,
        std::uint64_t monotonic_deadline_ms) = 0;
};

Result<BestEffortCheckpointResult> run_best_effort_checkpoints(
    IServiceShutdownActions& actions,
    bool enabled,
    std::uint64_t monotonic_deadline_ms);
Result<ControlledStopResult> run_controlled_stop(
    IServiceShutdownActions& actions,
    std::uint64_t monotonic_deadline_ms);

struct WorkerLaunchRequest final {
    std::wstring executable_path;
    product::Uuid profile_id;
    product::Uuid image_id;
    std::uint64_t session_generation{};
    std::uint64_t profile_revision{};
#if defined(MYRD_ENABLE_ENGINEERING_COMMANDS)
    bool engineering_exit_after_startup{};
    bool engineering_pipe_probe{};
    bool allow_unprotected_engineering_path{};
#endif
};

struct WorkerStartupAttachment final {
    host::ServiceReattachmentResult connection;
    host::WorkerRuntimeStatus status;
};

class WorkerProcess final {
public:
    WorkerProcess(WorkerProcess&&) noexcept;
    WorkerProcess& operator=(WorkerProcess&&) noexcept;
    ~WorkerProcess() noexcept;
    WorkerProcess(const WorkerProcess&) = delete;
    WorkerProcess& operator=(const WorkerProcess&) = delete;

    const host::WorkerBinding& binding() const noexcept;
    bool is_running() const noexcept;
    Result<WorkerStartupAttachment> attach_startup(
        std::uint32_t timeout_ms);
    Result<void> authenticate_startup(std::uint32_t timeout_ms);
    Result<std::uint32_t> wait(std::uint32_t timeout_ms) noexcept;

private:
    struct Impl;
    explicit WorkerProcess(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;

    friend Result<WorkerProcess> launch_worker(
        const WorkerLaunchRequest& request);
};

Result<WorkerProcess> launch_worker(const WorkerLaunchRequest& request);

#if defined(MYRD_ENABLE_TEST_SEAMS)
bool live_reconnect_status_is_safe_for_test(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerRuntimeStatus& next) noexcept;
bool live_reconnect_binding_is_safe_for_test(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerBinding& managed,
    const host::WorkerBinding& visible) noexcept;
bool managed_live_status_is_safe_for_test(
    const host::WorkerRuntimeStatus& previous,
    const host::WorkerBinding& managed,
    const host::WorkerRuntimeStatus& next) noexcept;
Result<std::uint32_t> scm_pending_status_pulse_count_for_test();
Result<std::uint32_t> scm_stop_pending_covers_prework_for_test();
Result<std::uint32_t> scm_stop_request_claim_lifecycle_for_test();
#endif

class WindowsProcessIdentityQuery final
    : public IWorkerProcessIdentityQuery {
public:
    Result<std::unique_ptr<IWorkerProcessIdentityPin>> pin_process(
        std::uint32_t process_id) override;
};

int run_service_program(int argc, wchar_t** argv) noexcept;

} // namespace myrd::management::windows
