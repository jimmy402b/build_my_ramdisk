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
    std::wstring volume_guid;
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
    Result<WorkerRuntimeStatus> inspect_bundle() override;
    Result<WorkerPreparedCandidate> prepare_checkpoint(
        std::uint64_t native_operation_id) override;
    Result<void> freeze_writes() override;
    Result<void> release_write_freeze() override;
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

    // Reads the image out of the bundle, checks it against the one the launch
    // named when there is one, and records the evidence the service registers.
    //
    // One implementation for both callers on purpose: `start()` and the
    // read-only inspection must verify the same things and report them the same
    // way, and two copies would be two chances for an inspection to produce
    // evidence a start would have refused.
    Result<void> record_bundle_identity();

    // The two absences a forget has to prove, answered by the only command that
    // opens this bundle without a runtime: whether the writer authority can be
    // taken (so nothing else holds it) and whether any proxy device has this
    // bundle's geometry.
    Result<WorkerForgetProbe> probe_forget_absences();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace myrd::m5
