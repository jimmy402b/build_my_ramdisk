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
#include "src/host/m5_worker_private_pipe.h"
#include "src/host/m5_worker_session.h"
#include "src/persistence/image_inspector.h"
#include "src/persistence/recovery_engine.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace myrd::m5 {

struct WorkerRuntimeConfig final {
    std::filesystem::path bundle;
    std::string manifest_name;
    // The image this bundle holds, or must be created to hold.
    //
    // `provision_new` creates generation zero *with* it, so the service's
    // derived image id and the bundle's own are one fact rather than two that
    // have to agree later.  `adopt_existing` checks it against the bundle's own
    // bytes and refuses to run when they disagree.  Absent means "record
    // whatever the bundle holds", which is what an import asks for -- and it is
    // refused for a create, where there is nothing yet to record.
    std::optional<persistence::ImageUuid> image_id;
    std::uint64_t virtual_bytes{};
    wchar_t drive_letter{};
    std::wstring owner_token;
    // What `start()` does with the volume.  Declared beside the image rather
    // than near the end because the two are read together: between them they
    // say which of the two lifecycles this worker runs.
    WorkerVolumeMode volume_mode{WorkerVolumeMode::adopt_existing};
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
    Result<WorkerCheckpointCommitReply> commit_checkpoint(
        const WorkerCheckpointCommit& commit) override;
    Result<WorkerCheckpointAbortReply> abort_checkpoint(
        std::uint64_t native_operation_id) override;
    Result<WorkerRecoveryProof> prove_recovery(
        const WorkerRecoveryCorrelation& correlation) override;
    Result<WorkerRemoveAndStopReply> remove_and_stop(
        const WorkerRemovalAuthorization& authorization) override;
    Result<void> shutdown() override;
    Result<void> cleanup() override;

private:
    explicit DiskWorkerRuntime(WorkerRuntimeConfig config) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace myrd::m5
