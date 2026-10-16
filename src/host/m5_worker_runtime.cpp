#include "src/host/m5_worker_runtime.h"

#include "src/common/diagnostic_trace.h"
#include "src/m3/m3_provider.h"
#include "src/run/runtime_adapter.h"

#include <chrono>
#include <utility>

namespace myrd::m5 {
namespace {

// The trace itself now lives in `src/common/diagnostic_trace.h`, because the steps worth
// naming are no longer only this layer's: the block turned out to be inside M3's own start
// sequence, and `src/m3` may not depend on a worker-private function.  This is a thin adapter
// so the call sites below keep reading in terms of the reclaim.
//
// The variable it responds to is `MYRD_TRACE_FILE`.
void reclaim_trace(const wchar_t* step) {
    // Narrowed because the shared facility takes bytes and the call sites are wide string
    // literals; every step name here is ASCII, so nothing can be lost.
    char name[128]{};
    const int converted =
        WideCharToMultiByte(CP_UTF8, 0, step, -1, name, sizeof(name) - 1,
                            nullptr, nullptr);
    if (converted <= 0) return;
    diagnostic_trace(name);
}

// Wraps the reclaim's start sequence so each step is recorded before and after it runs.
//
// The names are the `DiskRuntime` steps, in the order that runtime calls them, because the
// question is *which* of them blocks -- so the trace has to be in their terms rather than in
// the adapter's.  A step that logs a start and no finish is the one that hung.
void trace_reclaim_steps(DiskRuntimeOperations& operations) {
    // `acquire_bundle_lock` takes the bundle path, so it is wrapped on its own rather than
    // through a shared helper -- the signatures differ and forcing one would need a
    // template argument that buys nothing here.
    if (operations.acquire_bundle_lock) {
        auto inner = operations.acquire_bundle_lock;
        operations.acquire_bundle_lock =
            [inner](const std::filesystem::path& path) {
                reclaim_trace(L"acquire_bundle_lock: begin");
                auto result = inner(path);
                reclaim_trace(result.ok() ? L"  ok" : L"  refused");
                return result;
            };
    }
    // `recover_complete_image` and `start_aim` take nothing but bytes (or nothing at all),
    // and the two after them take a letter and a volume; each is wrapped in its own terms.
    if (operations.recover_complete_image) {
        auto inner = operations.recover_complete_image;
        operations.recover_complete_image = [inner](const std::uint64_t bytes) {
            reclaim_trace(L"recover_complete_image: begin");
            auto result = inner(bytes);
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }
    if (operations.start_aim) {
        auto inner = operations.start_aim;
        operations.start_aim = [inner] {
            // `start_aim` is one step in M4's terms and several in M3's, and the trace above
            // proved the difference matters: when the whole step is `begin` with no `ok`, the
            // reading cannot say *which* part of bringing AIM up blocked.  These markers name
            // the parts, so one guest run still answers the question.
            reclaim_trace(L"start_aim: begin");
            reclaim_trace(L"  -> provider open + start");
            auto result = inner();
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }
    if (operations.find_recovered_volume) {
        auto inner = operations.find_recovered_volume;
        operations.find_recovered_volume = [inner](const wchar_t letter) {
            reclaim_trace(L"find_recovered_volume: begin");
            auto result = inner(letter);
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }
    if (operations.verify_volume) {
        auto inner = operations.verify_volume;
        operations.verify_volume = [inner](const ProductVolume& volume,
                                           const std::uint64_t bytes) {
            reclaim_trace(L"verify_volume: begin");
            auto result = inner(volume, bytes);
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }

    // **The teardown is traced too, because a reclaim now gets all the way through the adopt
    // path and then stops.**  The trace ended between `verify_volume: ok` and the reclaim
    // command, which is the whole dismantle sequence -- four steps, and the reading could not
    // say which.  Since a reclaim's entire purpose is that sequence, an unreadable teardown is
    // the one place a trace is worth most.
    if (operations.remove_drive_letter) {
        auto inner = operations.remove_drive_letter;
        operations.remove_drive_letter = [inner](const ProductVolume& volume) {
            reclaim_trace(L"remove_drive_letter: begin");
            auto result = inner(volume);
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }
    if (operations.dismount_volume) {
        auto inner = operations.dismount_volume;
        operations.dismount_volume = [inner](const ProductVolume& volume,
                                             std::optional<LockedVolume>& held) {
            reclaim_trace(L"dismount_volume: begin");
            auto result = inner(volume, held);
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }
    if (operations.stop_aim) {
        auto inner = operations.stop_aim;
        operations.stop_aim = [inner] {
            reclaim_trace(L"stop_aim: begin");
            auto result = inner();
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }
    if (operations.release_bundle_lock) {
        auto inner = operations.release_bundle_lock;
        operations.release_bundle_lock = [inner] {
            reclaim_trace(L"release_bundle_lock: begin");
            auto result = inner();
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }
    if (operations.flush_volume) {
        auto inner = operations.flush_volume;
        operations.flush_volume = [inner](const ProductVolume& volume,
                                          std::optional<LockedVolume>& held) {
            reclaim_trace(L"flush_volume: begin");
            auto result = inner(volume, held);
            reclaim_trace(result.ok() ? L"  ok" : L"  refused");
            return result;
        };
    }
}

} // namespace

constexpr auto kWorkerOperationTimeout = std::chrono::minutes(5);

OperationContext context() {
    return {{}, std::chrono::steady_clock::now() + kWorkerOperationTimeout};
}

Error runtime_error(const wchar_t* operation, const wchar_t* message) {
    return {ErrorDomain::internal,
            static_cast<std::uint32_t>(OperationErrorCode::internal_invariant),
            operation, message, 0};
}

// The one place the run layer's commit disposition becomes the protocol's.
//
// One function, because there is one producer: this worker is the only thing
// that turns a committed-or-not fact into a commit reply, and a second mapping
// anywhere would be a second place for the four cases to be folded back
// together.
WorkerCommitDisposition protocol_disposition(
    const DiskCheckpointDisposition disposition) noexcept {
    switch (disposition) {
    case DiskCheckpointDisposition::committed:
        return WorkerCommitDisposition::committed;
    case DiskCheckpointDisposition::committed_needs_reconciliation:
        return WorkerCommitDisposition::committed_needs_reconciliation;
    case DiskCheckpointDisposition::commit_indeterminate:
        return WorkerCommitDisposition::commit_indeterminate;
    case DiskCheckpointDisposition::not_committed:
        return WorkerCommitDisposition::not_committed;
    }
    // Unreachable for the four declared cases; an added run-layer disposition
    // falls here.  `commit_indeterminate` is the only safe answer: reading it as
    // landed would promote a commit that may not have happened, and reading it
    // as not-committed would license restoring a prior record over bytes that
    // may really be on disk.
    return WorkerCommitDisposition::commit_indeterminate;
}

persistence::RecoveryOptions default_recovery_options() {
    // The worker is told its bundle and size by the service, but not its
    // recovery limits: those are a property of the reader, not of the operation.
    // These are the values the M4/M3 test paths use, written as literals so this
    // file does not have to reach into the journal and VHDX format headers for
    // their names.
    constexpr std::uint64_t kMiB = 1024 * 1024;
    persistence::RecoveryOptions options{};
    options.limits = {64 * kMiB, 64 * kMiB, 8192, 64 * 4096, 64,
                      64 * 4096, 64, 2 * kMiB, 2 * kMiB};
    return options;
}

struct DiskWorkerRuntime::Impl final {
    explicit Impl(WorkerRuntimeConfig value) : config(std::move(value)) {}

    WorkerRuntimeConfig config;
    std::unique_ptr<ProductionRuntimeAdapter> adapter;
    std::unique_ptr<DiskRuntime> runtime;
    // The image the bundle turned out to hold, read from the bundle itself and
    // -- for an adopt -- checked against the one the service named.  Seeded from
    // the claim so a status read before `start()` describes the profile rather
    // than reporting an identity nobody has looked at yet, and replaced by the
    // verified one the moment there is one.
    persistence::ImageUuid observed_image_id{};
    // Engaged only after `start()` has confirmed the bundle's own image uuid.
    bool identity_verified{};
    bool cleaned_up{};
    // Captured during `start()`, from the inspection that verified the image and
    // from one read-attribute handle to the bundle directory.  Engaged only when
    // all of it was read: a partial record would be a registration the service
    // could never re-verify, so it is reported as no evidence at all.
    std::optional<WorkerBundleEvidence> bundle_evidence;
};

// Reads the identity of the bundle *directory*, for the durable registration.
//
// A second handle with FILE_READ_ATTRIBUTES and full sharing: the runtime
// already holds the writer lock through its own handle, and asking for anything
// stronger here would deadlock the worker against itself.  The flags match the
// management layer's `inspect_bundle_identity`, which reads the same identity
// for the same purpose.
Result<WorkerBundleEvidence> capture_bundle_evidence(
    const std::filesystem::path& bundle,
    const persistence::SelectedImageInspection& selected) {
    WorkerBundleEvidence evidence{};
    evidence.image_id = selected.image_uuid;
    evidence.virtual_bytes = selected.geometry.virtual_bytes;
    evidence.logical_sector_size = selected.geometry.logical_sector_size;
    evidence.physical_sector_size = selected.geometry.physical_sector_size;

    const HANDLE raw = CreateFileW(
        bundle.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        return Result<WorkerBundleEvidence>::failure(runtime_error(
            L"DiskWorkerRuntime::start",
            L"the bundle cannot be opened for identity inspection"));
    }
    auto identity = persistence::query_windows_file_identity(raw);
    static_cast<void>(CloseHandle(raw));
    if (!identity.ok()) {
        return Result<WorkerBundleEvidence>::failure(identity.error());
    }
    evidence.volume_serial = identity.value().volume_serial_number;
    evidence.file_id = identity.value().file_id;
    if (!evidence.complete()) {
        return Result<WorkerBundleEvidence>::failure(runtime_error(
            L"DiskWorkerRuntime::start",
            L"the bundle's identity is incomplete and must not be recorded"));
    }
    return Result<WorkerBundleEvidence>::success(std::move(evidence));
}

DiskWorkerRuntime::DiskWorkerRuntime(WorkerRuntimeConfig config) noexcept
    : impl_(std::make_unique<Impl>(std::move(config))) {
    if (impl_->config.image_id.has_value()) {
        impl_->observed_image_id = *impl_->config.image_id;
    }
}

DiskWorkerRuntime::~DiskWorkerRuntime() noexcept = default;

Result<std::unique_ptr<DiskWorkerRuntime>> DiskWorkerRuntime::open(
    WorkerRuntimeConfig config) {
    if (config.bundle.empty() || config.manifest_name.empty()) {
        return Result<std::unique_ptr<DiskWorkerRuntime>>::failure(
            runtime_error(L"DiskWorkerRuntime::open",
                          L"the worker has no bundle to manage"));
    }
    if (config.recovery.limits.max_journal_committed_bytes == 0) {
        config.recovery = default_recovery_options();
    }
    // Opening the provider is deferred to `start()`.  The worker must not touch
    // a bundle, an AIM device or a volume before the handshake completes
    // (solution ?2.4), and this call is made before the handshake.
    return Result<std::unique_ptr<DiskWorkerRuntime>>::success(
        std::unique_ptr<DiskWorkerRuntime>(
            new DiskWorkerRuntime(std::move(config))));
}

Result<void> DiskWorkerRuntime::start() {
    auto& impl = *impl_;
    if (impl.runtime) {
        return Result<void>::failure(
            runtime_error(L"DiskWorkerRuntime::start",
                          L"the worker runtime has already been started"));
    }
    const bool provision_new =
        impl.config.volume_mode == WorkerVolumeMode::provision_new;
    if (provision_new && !impl.config.image_id.has_value()) {
        // A create has no bundle to inspect, so an absent image id would leave
        // the worker inventing the durable identity the service is going to
        // record.  Refused here rather than discovered by the service afterwards
        // refusing evidence it cannot match.
        return Result<void>::failure(runtime_error(
            L"DiskWorkerRuntime::start",
            L"a new bundle cannot be created without the image it must hold"));
    }
    impl.adapter = std::make_unique<ProductionRuntimeAdapter>(
        ProductionRuntimeAdapter::Config{
            impl.config.bundle, impl.config.virtual_bytes,
            impl.config.drive_letter, impl.config.owner_token, 0,
            impl.config.image_id,
            // The one place that knows this worker is dismantling rather than serving, handed down so
            // M3 can skip the physical-drive wait it would otherwise pay for a value nobody reads.
            impl.config.volume_mode == WorkerVolumeMode::reclaim_existing});
    RuntimeAdapterDependencies dependencies{
        impl.config.bundle,
        impl.config.virtual_bytes,
        impl.config.drive_letter,
        *impl.adapter,
        *impl.adapter,
        impl.config.owner_token,
        make_production_environment()};
    auto operations = make_runtime_operations(dependencies);
    if (!operations.ok()) {
        return Result<void>::failure(operations.error());
    }
    // **A reclaim gets a per-step trace, and only a reclaim.**  The measured symptom is a
    // reclaim that blocks for the whole command budget while its process consumes almost no
    // CPU -- a blocked system call somewhere in the start sequence -- and neither the
    // service nor the worker can say which step, because the reply never comes back.  A
    // trace file is the only way to see inside a worker that is not answering.
    //
    // It is applied here rather than in `make_runtime_operations` for two reasons: this is
    // the layer that knows which mode is running, so a create or a recovery is untouched;
    // and `runtime_adapter.cpp` is shared with the foreground CLI, which must not gain
    // tracing for a defect that only the service's reclaimer has.
    //
    // Gated on `MYRD_RECLAIM_TRACE` naming a file, so production pays a `GetEnvironmentVariableW`
    // per step and nothing else, and a run without the variable behaves exactly as before.
    if (impl.config.volume_mode == WorkerVolumeMode::reclaim_existing) {
        trace_reclaim_steps(operations.value());
    }
    DiskRuntimeOptions options{};
    options.bundle = impl.config.bundle;
    options.virtual_bytes = impl.config.virtual_bytes;
    options.drive_letter = impl.config.drive_letter;
    // Which lifecycle this worker runs, chosen by the launch and not by what it finds:
    // `DiskRuntime` independently requires the bundle's own kind to match the mode, so a
    // mode that disagrees with reality fails closed inside the runtime rather than
    // silently taking the other path.
    //
    // `reclaim_existing` is the third option, and it maps to `DiskRuntime::reclaim` --
    // which shares the attach-only find with `recover` and differs in the one way that
    // matters here: it assigns no drive letter, because the leftover already holds the
    // one being requested.
    auto runtime = impl.config.volume_mode == WorkerVolumeMode::provision_new
        ? DiskRuntime::create(options, operations.move_value())
        : (impl.config.volume_mode == WorkerVolumeMode::reclaim_existing
               ? DiskRuntime::reclaim(options, operations.move_value())
               : DiskRuntime::recover(options, operations.move_value()));
    if (!runtime.ok()) return Result<void>::failure(runtime.error());
    impl.runtime = runtime.move_value();

    auto started = impl.runtime->start();
    if (!started.ok()) {
        // A failed start owns no later resources, but it may own earlier ones;
        // the runtime's own cleanup is reverse-order and idempotent.
        static_cast<void>(impl.runtime->shutdown_clean());
        return started;
    }

    auto recorded = record_bundle_identity();
    if (!recorded.ok()) {
        static_cast<void>(impl.runtime->shutdown_clean());
        return recorded;
    }
    impl.identity_verified = true;
    return Result<void>::success();
}

Result<void> DiskWorkerRuntime::record_bundle_identity() {
    auto& impl = *impl_;
    // The identity is taken from the bundle's own bytes, never from the claim.
    // A worker that took the service's word for which image it is running could
    // then be asked to remove resources belonging to a different one.
    persistence::ImageInspectOptions inspect{};
    inspect.manifest_relative_name = impl.config.manifest_name;
    inspect.recovery = impl.config.recovery;
    auto image = persistence::inspect_image(context(), impl.config.bundle,
                                            inspect);
    if (!image.ok()) return Result<void>::failure(image.error());
    if (!image.value().selected.has_value()) {
        return Result<void>::failure(runtime_error(
            L"DiskWorkerRuntime::record_bundle_identity",
            L"the bundle holds no image this worker can verify"));
    }
    // A claim is verified; an absent one is adopted.  The difference is the
    // whole of what `read_only_inspection` means: an import records the bundle
    // it found, while every other launch names the image it expects and is
    // refused when the bundle is a different one.
    if (impl.config.image_id.has_value() &&
        !(image.value().selected->image_uuid == *impl.config.image_id)) {
        return Result<void>::failure(runtime_error(
            L"DiskWorkerRuntime::record_bundle_identity",
            L"the bundle's image uuid is not the one this worker was launched for"));
    }
    // The evidence the service records as its durable registration.  Captured
    // here because this is the one place the bundle is known to be the one the
    // service named -- the uuid above has just been checked against it -- and
    // both the start and the read-only inspection come through it.
    auto evidence = capture_bundle_evidence(
        impl.config.bundle, *image.value().selected);
    if (!evidence.ok()) return Result<void>::failure(evidence.error());
    impl.observed_image_id = image.value().selected->image_uuid;
    impl.bundle_evidence = evidence.move_value();
    return Result<void>::success();
}

Result<WorkerRuntimeStatus> DiskWorkerRuntime::inspect_bundle() {
    auto& impl = *impl_;
    // A runtime that is already up opened and verified the bundle in `start()`,
    // so the evidence is in hand: re-inspecting would take a second handle on a
    // file this worker holds the writer lock for, and would report a bundle the
    // worker is running as one it merely looked at.
    if (!impl.bundle_evidence.has_value()) {
        auto recorded = record_bundle_identity();
        if (!recorded.ok()) {
            return Result<WorkerRuntimeStatus>::failure(recorded.error());
        }
    }
    // `observe()` is what reports the evidence, and it reports it whether or not
    // the runtime is alive -- here it is not, which is exactly the answer an
    // inspection gives.  Nothing is started, so nothing is claimed.
    return observe();
}

Result<WorkerRuntimeStatus> DiskWorkerRuntime::observe() {
    auto& impl = *impl_;
    WorkerRuntimeStatus status{};
    status.worker_alive = impl.runtime != nullptr && !impl.cleaned_up;
    // The identity the worker verified for itself, which is what a removal
    // authorization is checked against.  Reporting the launch's claim here
    // instead would let the service authorize a removal against an image the
    // worker never confirmed it holds.
    status.image_id = impl.observed_image_id;
    if (impl.runtime) {
        const auto stats = impl.runtime->maintenance_stats();
        status.committed_generation = stats.generation;
        status.dirty = stats.dirty_bytes != 0;
        // Reported from the runtime that holds it, not inferred from the fact
        // that a freeze was once requested: the two answers differ exactly when
        // it matters, which is when a stop has to know whether the removal it is
        // about to authorize is protected by a freeze or merely preceded by one.
        status.writes_frozen = impl.runtime->writes_frozen();
        // The identities the service records into the durable profile come from
        // the volume this runtime actually adopted -- and only when all three
        // were read.  An incomplete triple is not a weaker identity: it is one
        // no later start could verify against, so it is reported as no identity
        // at all rather than as a profile the service would happily write.
        if (const auto adopted = impl.runtime->adopted_volume();
            adopted.has_value() && adopted->identity.complete()) {
            status.volume = WorkerVolumeIdentity{adopted->identity.disk_guid,
                                                 adopted->identity.partition_guid,
                                                 adopted->identity.volume_guid};
        }
    }
    // The bundle evidence does not depend on the runtime being alive: it
    // describes the file object this worker opened and verified, which is still
    // the same object after a teardown.  Reported whenever it was captured.
    status.bundle = impl.bundle_evidence;
    return Result<WorkerRuntimeStatus>::success(status);
}

Result<void> DiskWorkerRuntime::freeze_writes() {
    auto& impl = *impl_;
    if (!impl.runtime || !impl.identity_verified) {
        return Result<void>::failure(runtime_error(
            L"DiskWorkerRuntime::freeze_writes",
            L"the worker runtime is not running"));
    }
    // Idempotent at this layer too, and for the same reason it is at the runtime:
    // a service that retries the freeze after a transport failure is asking for a
    // state, not for a second lock, and a second lock is not possible.
    return impl.runtime->freeze_writes();
}

Result<void> DiskWorkerRuntime::release_write_freeze() {
    auto& impl = *impl_;
    if (!impl.runtime) return Result<void>::success();
    return impl.runtime->release_write_freeze();
}

Result<WorkerPreparedCandidate> DiskWorkerRuntime::prepare_checkpoint(
    const std::uint64_t native_operation_id) {
    auto& impl = *impl_;
    if (!impl.runtime || !impl.identity_verified) {
        return Result<WorkerPreparedCandidate>::failure(runtime_error(
            L"DiskWorkerRuntime::prepare_checkpoint",
            L"the worker runtime is not running"));
    }
    // The mounted filesystem has to be flushed before persistence cuts its
    // checkpoint, exactly as the foreground M4 loop does.
    auto flushed = impl.runtime->flush_volume();
    if (!flushed.ok()) {
        return Result<WorkerPreparedCandidate>::failure(flushed.error());
    }
    DiskCheckpointPrepareRequest request{};
    request.native_operation_id = native_operation_id;
    auto prepared = impl.runtime->prepare_checkpoint(request);
    if (!prepared.ok()) {
        return Result<WorkerPreparedCandidate>::failure(prepared.error());
    }
    WorkerPreparedCandidate candidate{};
    candidate.prior_generation = prepared.value().prior_generation;
    candidate.candidate_generation = prepared.value().candidate_generation;
    candidate.native_operation_id = native_operation_id;
    candidate.cut_dirty_epoch = impl.runtime->maintenance_stats().generation;
    candidate.manifest_slot = prepared.value().manifest_slot;
    candidate.candidate_bytes_digest = prepared.value().candidate_bytes_digest;
    return Result<WorkerPreparedCandidate>::success(candidate);
}

Result<WorkerCheckpointCommitReply> DiskWorkerRuntime::commit_checkpoint(
    const WorkerCheckpointCommit& commit) {
    auto& impl = *impl_;
    if (!impl.runtime) {
        return Result<WorkerCheckpointCommitReply>::failure(runtime_error(
            L"DiskWorkerRuntime::commit_checkpoint",
            L"the worker runtime is not running"));
    }
    DiskCheckpointCommitRequest request{};
    request.native_operation_id = commit.native_operation_id;
    request.manifest_slot = commit.manifest_slot;
    request.candidate_bytes_digest = commit.candidate_bytes_digest;
    auto committed = impl.runtime->commit_checkpoint(request);
    if (!committed.ok()) {
        return Result<WorkerCheckpointCommitReply>::failure(committed.error());
    }
    const auto& result = committed.value();
    WorkerCheckpointCommitReply reply{};
    // Relayed verbatim, including the three dispositions that are not
    // `committed`.  They are *not* derived from the observed generation: the
    // generation says where the runtime now stands and the disposition says what
    // the publish did, and precisely when the two can disagree is the moment the
    // difference is the whole question.  Folding them into one error was the
    // defect this replaces.
    reply.disposition = protocol_disposition(result.disposition);
    reply.generation = result.generation;
    if (!reply.landed()) {
        // A commit that did not cleanly land must carry the failure that decided
        // it; the service validates its own durable evidence against this
        // record, and it refuses a non-landed commit with no failure evidence.
        if (!result.error_context.has_value()) {
            return Result<WorkerCheckpointCommitReply>::failure(runtime_error(
                L"DiskWorkerRuntime::commit_checkpoint",
                L"the checkpoint did not commit and reported no failure evidence"));
        }
        const auto& context = *result.error_context;
        WorkerCommitFailure failure{};
        failure.operation = context.operation;
        failure.code = context.code;
        failure.phase = context.phase;
        failure.native_code = context.native_code;
        failure.native_operation_id = context.native_operation_id;
        failure.image_uuid.bytes = context.image_uuid;
        failure.generation = context.generation;
        failure.safe_context = context.safe_context;
        reply.failure = std::move(failure);
    }
    auto status = observe();
    if (!status.ok()) return Result<WorkerCheckpointCommitReply>::failure(
        status.error());
    reply.status = status.move_value();
    return Result<WorkerCheckpointCommitReply>::success(std::move(reply));
}

Result<WorkerCheckpointAbortReply> DiskWorkerRuntime::abort_checkpoint(
    const std::uint64_t native_operation_id) {
    auto& impl = *impl_;
    if (!impl.runtime) {
        return Result<WorkerCheckpointAbortReply>::failure(runtime_error(
            L"DiskWorkerRuntime::abort_checkpoint",
            L"the worker runtime is not running"));
    }
    auto aborted = impl.runtime->abort_checkpoint(native_operation_id);
    WorkerCheckpointAbortReply reply{};
    // The generation the runtime stands at either way.  A successful abort must
    // leave it where it was, and the caller checks exactly that -- so the number
    // is reported on both paths, not only the successful one.
    reply.generation = impl.runtime->maintenance_stats().generation;
    if (!aborted.ok()) {
        // A failed abort is reported as a refusal with a reason rather than as
        // a transport failure.  There is no persistence context to relay --
        // M3Provider::abort_checkpoint and the transaction abort under it both
        // return Result<void> -- so the reason travels as a value, and the
        // caller gets the one thing it can act on.  See the protocol's comment
        // on WorkerAbortRefusal for why that is the honest shape rather than a
        // context composed from the bare error.
        reply.aborted = false;
        reply.refusal = WorkerAbortRefusal::abort_failed;
        return Result<WorkerCheckpointAbortReply>::success(std::move(reply));
    }
    reply.aborted = true;
    return Result<WorkerCheckpointAbortReply>::success(std::move(reply));
}

Result<WorkerRecoveryProof> DiskWorkerRuntime::prove_recovery(
    const WorkerRecoveryCorrelation& correlation) {
    auto& impl = *impl_;
    // Deliberately callable before `start()`: the whole point of a recovery
    // proof is that a fresh worker decides what the bundle proves *before* it
    // takes the writer lock.  The M3 entry point is static and takes no lock,
    // no store and no runtime for exactly this reason.
    m3::M3ProviderConfig config{};
    config.bundle_root = impl.config.bundle;
    config.manifest_name = impl.config.manifest_name;
    config.recovery = impl.config.recovery;
    auto inspection = m3::M3Provider::inspect_checkpoint(
        context(), config, correlation.checkpoint);
    if (!inspection.ok()) {
        return Result<WorkerRecoveryProof>::failure(inspection.error());
    }
    WorkerRecoveryProof proof{};
    // The mapping is the one shared function, so the protocol's spelling cannot
    // drift from the read side's verdict.
    proof.outcome = worker_recovery_outcome(inspection.value().verdict);
    proof.observed_generation = inspection.value().candidate_generation;
    proof.native_operation_id = inspection.value().journal_operation_id;
    proof.manifest_slot = inspection.value().selected_manifest_slot;
    proof.candidate_bytes_digest = inspection.value().selected_manifest_digest;
    return Result<WorkerRecoveryProof>::success(proof);
}

Result<WorkerRemoveAndStopReply> DiskWorkerRuntime::remove_and_stop(
    const WorkerRemovalAuthorization& authorization) {
    auto& impl = *impl_;
    // Re-check ownership here rather than trusting the session's gate: this is
    // the call that actually dismantles things, and a second check at the point
    // of no return is cheap.  A partial proof never reaches it.
    if (!authorization.writer_lock_proved || !authorization.device_match_proved ||
        !authorization.bundle_identity_proved ||
        !(authorization.image_id == impl.observed_image_id)) {
        return Result<WorkerRemoveAndStopReply>::failure(runtime_error(
            L"DiskWorkerRuntime::remove_and_stop",
            L"the removal authorization does not cover this worker's own image"));
    }

    // Refuse a dirty removal, and refuse it *here*.
    //
    // The removal barrier drops every uncommitted write with the mount, so a
    // removal performed over a dirty runtime is indistinguishable from data
    // loss.  This is the check a stop depends on, and it belongs in the
    // authority that actually dismantles rather than in the caller that asks: a
    // guard in the caller protects only that caller, and every other path to
    // this call -- the service composition, a retry, a future executor -- would
    // silently bypass it.  Checked before `cleanup()`, because teardown is what
    // would flush the very writes this is refusing to drop.
    //
    // The refusal is returned as a *reply*, not as an error.  It is a decision
    // this runtime reached about its own state, the caller has to act on the
    // reason, and an error would be indistinguishable from a broken channel --
    // which is exactly the answer a caller must not retry.
    WorkerRemoveAndStopReply reply{};
    auto before = observe();
    if (!before.ok()) {
        return Result<WorkerRemoveAndStopReply>::failure(before.error());
    }
    reply.generation = before.value().committed_generation;
    if (before.value().dirty) {
        reply.refusal = WorkerRemovalRefusal::runtime_dirty;
        return Result<WorkerRemoveAndStopReply>::success(std::move(reply));
    }

    auto cleaned = cleanup();
    if (!cleaned.ok()) {
        return Result<WorkerRemoveAndStopReply>::failure(cleaned.error());
    }
    reply.removed = true;
    reply.stopped = true;
    return Result<WorkerRemoveAndStopReply>::success(std::move(reply));
}

Result<void> DiskWorkerRuntime::shutdown() {
    // Shutdown asks the worker to go away and dismantles nothing: the ordered
    // teardown stays `cleanup`'s job, so a caller can still get a truthful
    // teardown report afterwards.  It also needs no runtime.
    return Result<void>::success();
}

Result<void> DiskWorkerRuntime::cleanup() {
    auto& impl = *impl_;
    if (impl.cleaned_up) return Result<void>::success();
    if (!impl.runtime) {
        impl.cleaned_up = true;
        return Result<void>::success();
    }
    auto stopped = impl.runtime->shutdown_clean();
    if (!stopped.ok()) {
        // A teardown that could not resolve a pending transaction retains the
        // runtime rather than reporting a clean stop it did not achieve.
        return Result<void>::failure(stopped.error());
    }
    impl.cleaned_up = true;
    impl.identity_verified = false;
    return Result<void>::success();
}

} // namespace myrd::m5
