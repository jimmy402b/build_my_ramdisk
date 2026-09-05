#pragma once

#include "src/host/host_ipc.h"
#include "src/management/service_core.h"
#include "src/management/worker_supervisor.h"
#include "src/profile/profile_types.h"

#include <string>

namespace myrd::management::windows {

class SystemServiceClock final : public IServiceClock {
public:
    std::uint64_t unix_time_ms() const noexcept override;
    std::uint64_t monotonic_time_ms() const noexcept override;
};

class DurableWorkerRestartHistoryStore final
    : public IWorkerRestartHistoryStore {
public:
    explicit DurableWorkerRestartHistoryStore(std::wstring program_data);

    Result<std::optional<WorkerRestartHistory>> load(
        const product::Uuid& profile_id) override;
    Result<void> write_next(
        const product::Uuid& profile_id,
        std::optional<std::uint64_t> expected_revision,
        const WorkerRestartHistory& next) override;
    Result<void> clear(const product::Uuid& profile_id);

private:
    std::wstring program_data_;
};

Result<host::WorkerRuntimeStatus> load_persisted_worker_status(
    const std::wstring& program_data,
    const host::WorkerBinding& expected_binding);

Result<product::Uuid> derive_worker_recovery_operation_id(
    const WorkerRuntimeIdentity& failed_worker);

Result<WorkerCheckpointRecoveryProof> reconcile_interrupted_checkpoint(
    const profile::ProfileRecord& record,
    const host::WorkerRuntimeStatus& persisted,
    const WorkerRestartRecoveryRequest& request);

Result<WorkerResidualDeviceRecoveryProof> reconcile_windows_residual_device(
    const host::WorkerRuntimeStatus& persisted,
    const WorkerRestartRecoveryRequest& request);

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<std::vector<std::byte>> encode_worker_restart_history_for_test(
    const WorkerRestartHistory& history);
Result<WorkerRestartHistory> decode_worker_restart_history_for_test(
    std::span<const std::byte> bytes,
    const product::Uuid& expected_profile);
#endif

} // namespace myrd::management::windows
