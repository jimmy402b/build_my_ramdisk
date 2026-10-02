#pragma once

// The production `WorkerRuntime`: the worker's own view of the bundle it was
// launched to manage.
//
// It is host-coupled on purpose, and that is why it is not part of
// `myrd_m5_worker_protocol` -- that target stays free of M3 and execution
// dependencies so the service-side adapter can link it.  This file belongs to
// the worker executable, which is the only place allowed to reach DiskRuntime
// and M3 (plan Task 0 Step 3).

#include "src/common/operation_context.h"
#include "src/common/result.h"
#include "src/host/m5_worker_session.h"
#include "src/persistence/image_inspector.h"
#include "src/persistence/recovery_engine.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace myrd::m5 {

struct WorkerRuntimeConfig final {
    std::filesystem::path bundle;
    std::string manifest_name;
    // The image the service recorded for this profile.  The worker does not
    // take this on trust: `start()` re-reads the bundle's own image uuid and
    // refuses to run when the two disagree, so `observe()` never reports an
    // identity the worker has not verified for itself.
    persistence::ImageUuid image_id{};
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    std::wstring owner_token;
    persistence::RecoveryOptions recovery{};
};

class DiskWorkerRuntime final : public WorkerRuntime {
public:
    [[nodiscard]] static Result<std::unique_ptr<DiskWorkerRuntime>> open(
        WorkerRuntimeConfig config);
    ~DiskWorkerRuntime() noexcept override;
    DiskWorkerRuntime(const DiskWorkerRuntime&) = delete;
    DiskWorkerRuntime& operator=(const DiskWorkerRuntime&) = delete;

    Result<void> start() override;
    Result<WorkerRuntimeStatus> observe() override;
    Result<WorkerPreparedCandidate> prepare_checkpoint(
        std::uint64_t native_operation_id) override;
    Result<WorkerRuntimeStatus> commit_checkpoint(
        const WorkerCheckpointCommit& commit) override;
    Result<WorkerRuntimeStatus> abort_checkpoint(
        std::uint64_t native_operation_id) override;
    Result<WorkerRecoveryProof> prove_recovery(
        const WorkerRecoveryCorrelation& correlation) override;
    Result<WorkerRuntimeStatus> remove_and_stop(
        const WorkerRemovalAuthorization& authorization) override;
    Result<void> shutdown() override;
    Result<void> cleanup() override;

private:
    explicit DiskWorkerRuntime(WorkerRuntimeConfig config) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace myrd::m5
