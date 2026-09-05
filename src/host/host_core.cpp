#include "src/host/host_core.h"
#include "src/m3/m3_provider.h"

#include <exception>
#include <utility>

namespace myrd::host {

class MutationCompletion final {
public:
    explicit MutationCompletion(HostCore& core) noexcept : core_(&core) {}
    ~MutationCompletion() noexcept;
    MutationCompletion(const MutationCompletion&) = delete;
    MutationCompletion& operator=(const MutationCompletion&) = delete;

private:
    HostCore* core_;
};

namespace {

Error core_error(
    HostCoreError code,
    const wchar_t* operation,
    const wchar_t* message,
    ErrorDomain domain = ErrorDomain::internal) {
    return Error{domain, static_cast<std::uint32_t>(code), operation, message};
}

template <typename Outcome>
Outcome failed_outcome(Error error) {
    Outcome outcome{};
    outcome.error = std::move(error);
    return outcome;
}

bool is_known(HostVolumeBarrierKind barrier) noexcept {
    switch (barrier) {
    case HostVolumeBarrierKind::windows_volume_flushed:
    case HostVolumeBarrierKind::raw_block_test_no_filesystem_cache:
        return true;
    }
    return false;
}

} // namespace

HostCompactionOutcome IHostProvider::compact(
    const OperationContext&,
    HostCompactionRequest,
    const HostCompactionBoundaryHandler&,
    const HostCompactionCutHandler&) {
    return failed_outcome<HostCompactionOutcome>(core_error(
        HostCoreError::adapter_invariant,
        L"IHostProvider::compact",
        L"The provider does not implement active compaction"));
}

HostCompactionOutcome IHostProvider::reconcile_compaction(
    const OperationContext&,
    const HostCompactionBoundaryEvidence&) {
    return failed_outcome<HostCompactionOutcome>(core_error(
        HostCoreError::adapter_invariant,
        L"IHostProvider::reconcile_compaction",
        L"The provider does not implement compaction reconciliation"));
}

HostCore::HostCore(
    WorkerBinding expected_binding,
    std::shared_ptr<IHostProviderFactory> provider_factory) noexcept
    : expected_binding_(std::move(expected_binding)),
      provider_factory_(std::move(provider_factory)) {}

Result<std::unique_ptr<HostCore>> HostCore::create(
    WorkerBinding expected_binding,
    std::shared_ptr<IHostProviderFactory> provider_factory) {
    if (!is_valid(expected_binding) || !provider_factory) {
        return Result<std::unique_ptr<HostCore>>::failure(core_error(
            HostCoreError::invalid_argument,
            L"HostCore::create",
            L"A valid exact worker binding and provider factory are required",
            ErrorDomain::usage));
    }
    return Result<std::unique_ptr<HostCore>>::success(
        std::unique_ptr<HostCore>(new HostCore(
            std::move(expected_binding), std::move(provider_factory))));
}

Result<void> HostCore::adopt_reattached_binding(
    const WorkerBinding& binding) {
    std::scoped_lock lock(mutex_);
    if (binding == expected_binding_) return Result<void>::success();
    const bool same_live_worker = is_valid(binding)
        && binding.profile_id == expected_binding_.profile_id
        && binding.image_id == expected_binding_.image_id
        && binding.worker_pid == expected_binding_.worker_pid
        && binding.process_creation_time
            == expected_binding_.process_creation_time
        && binding.profile_revision == expected_binding_.profile_revision
        && binding.product_major == expected_binding_.product_major
        && binding.product_minor == expected_binding_.product_minor
        && binding.product_patch == expected_binding_.product_patch
        && binding.session_generation
            > expected_binding_.session_generation;
    if (!same_live_worker) {
        return Result<void>::failure(core_error(
            HostCoreError::binding_mismatch,
            L"HostCore::adopt_reattached_binding",
            L"Reattachment changed immutable worker identity or did not advance the session generation",
            ErrorDomain::protocol));
    }
    if (active_mutation_.has_value()) {
        return Result<void>::failure(core_error(
            HostCoreError::mutation_busy,
            L"HostCore::adopt_reattached_binding",
            L"The worker binding cannot rotate during an active mutation",
            ErrorDomain::storage));
    }
    expected_binding_ = binding;
    return Result<void>::success();
}

Result<void> HostCore::begin_mutation(const HostCommand& command) {
    std::scoped_lock lock(mutex_);
    if (!(command.binding == expected_binding_)) {
        return Result<void>::failure(core_error(
            HostCoreError::binding_mismatch,
            L"HostCore::begin_mutation",
            L"The host command does not match this worker binding",
            ErrorDomain::protocol));
    }
    if (active_mutation_.has_value()) {
        return Result<void>::failure(core_error(
            HostCoreError::mutation_busy,
            L"HostCore::begin_mutation",
            L"Another host mutation is already active",
            ErrorDomain::storage));
    }
    active_mutation_ = command.operation_id;
    return Result<void>::success();
}

void HostCore::end_mutation() noexcept {
    std::scoped_lock lock(mutex_);
    active_mutation_.reset();
}

Result<std::shared_ptr<IHostProvider>> HostCore::current_provider(
    const WorkerBinding& binding) const {
    std::scoped_lock lock(mutex_);
    if (!(binding == expected_binding_)) {
        return Result<std::shared_ptr<IHostProvider>>::failure(core_error(
            HostCoreError::binding_mismatch,
            L"HostCore::current_provider",
            L"The query does not match this worker binding",
            ErrorDomain::protocol));
    }
    if (!provider_) {
        return Result<std::shared_ptr<IHostProvider>>::failure(core_error(
            HostCoreError::provider_unavailable,
            L"HostCore::current_provider",
            L"This host has no live provider",
            ErrorDomain::storage));
    }
    return Result<std::shared_ptr<IHostProvider>>::success(provider_);
}

HostStartOutcome HostCore::start(
    const HostCommand& command,
    const OperationContext& context) {
    const auto admitted = begin_mutation(command);
    if (!admitted.ok()) return failed_outcome<HostStartOutcome>(admitted.error());
    MutationCompletion completion(*this);

    {
        std::scoped_lock lock(mutex_);
        if (provider_) {
            return failed_outcome<HostStartOutcome>(core_error(
                HostCoreError::invalid_state,
                L"HostCore::start",
                L"This host already owns a provider",
                ErrorDomain::storage));
        }
    }

    Result<std::shared_ptr<IHostProvider>> opened =
        Result<std::shared_ptr<IHostProvider>>::failure(core_error(
            HostCoreError::provider_exception,
            L"HostCore::start",
            L"The provider factory did not complete"));
    try {
        opened = provider_factory_->open(context);
    } catch (...) {
        return failed_outcome<HostStartOutcome>(core_error(
            HostCoreError::provider_exception,
            L"HostCore::start",
            L"The provider factory threw an exception"));
    }
    if (!opened.ok()) return failed_outcome<HostStartOutcome>(opened.error());
    auto provider = opened.move_value();
    if (!provider) {
        return failed_outcome<HostStartOutcome>(core_error(
            HostCoreError::adapter_invariant,
            L"HostCore::start",
            L"The provider factory returned a null provider"));
    }

    HostStartOutcome outcome{};
    outcome.provider_opened = true;
    Result<void> started = Result<void>::failure(core_error(
        HostCoreError::provider_exception,
        L"HostCore::start",
        L"The provider start did not complete"));
    try {
        started = provider->start(context);
    } catch (...) {
        started = Result<void>::failure(core_error(
            HostCoreError::provider_exception,
            L"HostCore::start",
            L"The provider start threw an exception"));
    }
    if (!started.ok()) {
        outcome.error = started.error();
        try {
            outcome.cleanup = provider->stop(context, {});
        } catch (...) {
            outcome.cleanup = failed_outcome<HostStopOutcome>(core_error(
                HostCoreError::provider_exception,
                L"HostCore::start.cleanup",
                L"Provider cleanup threw after start failure"));
        }
        return outcome;
    }

    try {
        auto state = provider->state();
        auto generation = provider->generation();
        auto geometry = provider->geometry();
        if (!state.ok() || !generation.ok() || !geometry.ok()) {
            outcome.error = !state.ok()
                ? state.error()
                : (!generation.ok() ? generation.error() : geometry.error());
        } else if (state.value() != HostProviderState::ready) {
            outcome.error = core_error(
                HostCoreError::adapter_invariant,
                L"HostCore::start",
                L"Provider start succeeded without entering Ready state");
        } else {
            outcome.generation = generation.value();
            outcome.geometry = geometry.value();
        }
    } catch (...) {
        outcome.error = core_error(
            HostCoreError::provider_exception,
            L"HostCore::start",
            L"Provider identity query threw after start");
    }
    if (outcome.error.has_value()) {
        try {
            outcome.cleanup = provider->stop(context, {});
        } catch (...) {
            outcome.cleanup = failed_outcome<HostStopOutcome>(core_error(
                HostCoreError::provider_exception,
                L"HostCore::start.cleanup",
                L"Provider cleanup threw after identity query failure"));
        }
        return outcome;
    }
    outcome.provider_started = true;
    {
        std::scoped_lock lock(mutex_);
        provider_ = std::move(provider);
    }
    return outcome;
}

HostCheckpointOutcome HostCore::checkpoint(
    const HostCommand& command,
    const OperationContext& context,
    HostVolumeBarrierKind barrier) {
    if (!is_known(barrier)) {
        return failed_outcome<HostCheckpointOutcome>(core_error(
            HostCoreError::adapter_invariant,
            L"HostCore::checkpoint",
            L"The checkpoint barrier is unknown",
            ErrorDomain::protocol));
    }
    const auto admitted = begin_mutation(command);
    if (!admitted.ok()) {
        return failed_outcome<HostCheckpointOutcome>(admitted.error());
    }
    MutationCompletion completion(*this);
    auto provider = current_provider(command.binding);
    if (!provider.ok()) {
        return failed_outcome<HostCheckpointOutcome>(provider.error());
    }
    try {
        return provider.value()->checkpoint(context, barrier);
    } catch (...) {
        return failed_outcome<HostCheckpointOutcome>(core_error(
            HostCoreError::provider_exception,
            L"HostCore::checkpoint",
            L"The provider checkpoint threw an exception"));
    }
}

HostCompactionOutcome HostCore::compact(
    const HostCommand& command,
    const OperationContext& context,
    const HostCompactionRequest request,
    const HostCompactionBoundaryHandler& boundary,
    const HostCompactionCutHandler& cut) {
    if (!is_known(request.barrier) ||
        request.native_operation_id == 0 || !boundary || !cut) {
        return failed_outcome<HostCompactionOutcome>(core_error(
            HostCoreError::invalid_argument,
            L"HostCore::compact",
            L"Active compaction requires a known barrier, correlation, and boundary handler",
            ErrorDomain::usage));
    }
    const auto admitted = begin_mutation(command);
    if (!admitted.ok()) {
        return failed_outcome<HostCompactionOutcome>(admitted.error());
    }
    MutationCompletion completion(*this);
    auto provider = current_provider(command.binding);
    if (!provider.ok()) {
        return failed_outcome<HostCompactionOutcome>(provider.error());
    }
    try {
        return provider.value()->compact(context, request, boundary, cut);
    } catch (...) {
        return failed_outcome<HostCompactionOutcome>(core_error(
            HostCoreError::provider_exception,
            L"HostCore::compact",
            L"The provider compaction threw an exception"));
    }
}

HostCompactionOutcome HostCore::reconcile_compaction(
    const HostCommand& command,
    const OperationContext& context,
    const HostCompactionBoundaryEvidence& boundary) {
    auto mapped = m3_adapter::map_compaction_boundary(boundary);
    if (!mapped.ok()) {
        return failed_outcome<HostCompactionOutcome>(mapped.error());
    }
    const auto admitted = begin_mutation(command);
    if (!admitted.ok()) {
        return failed_outcome<HostCompactionOutcome>(admitted.error());
    }
    MutationCompletion completion(*this);
    auto provider = current_provider(command.binding);
    if (!provider.ok()) {
        return failed_outcome<HostCompactionOutcome>(provider.error());
    }
    try {
        return provider.value()->reconcile_compaction(context, boundary);
    } catch (...) {
        return failed_outcome<HostCompactionOutcome>(core_error(
            HostCoreError::provider_exception,
            L"HostCore::reconcile_compaction",
            L"The provider compaction reconciliation threw"));
    }
}

HostStopOutcome HostCore::stop(
    const HostCommand& command,
    const OperationContext& context,
    HostStopRequest request) {
    const auto admitted = begin_mutation(command);
    if (!admitted.ok()) return failed_outcome<HostStopOutcome>(admitted.error());
    MutationCompletion completion(*this);
    if (request.checkpoint
        && (!request.barrier.has_value() || !is_known(*request.barrier))) {
        return failed_outcome<HostStopOutcome>(core_error(
            HostCoreError::invalid_argument,
            L"HostCore::stop",
            L"A checkpointing stop requires an explicit barrier",
            ErrorDomain::usage));
    }
    auto provider = current_provider(command.binding);
    if (!provider.ok()) return failed_outcome<HostStopOutcome>(provider.error());

    HostStopOutcome outcome{};
    try {
        outcome = provider.value()->stop(context, request);
    } catch (...) {
        outcome = failed_outcome<HostStopOutcome>(core_error(
            HostCoreError::provider_exception,
            L"HostCore::stop",
            L"The provider stop threw an exception"));
    }
    if (request.checkpoint && !outcome.checkpoint_completed
        && !outcome.error.has_value() && !outcome.error_context.has_value()) {
        outcome.error = core_error(
            HostCoreError::adapter_invariant,
            L"HostCore::stop",
            L"Provider omitted a requested checkpoint without failure evidence");
    }
    {
        std::scoped_lock lock(mutex_);
        if (provider_ == provider.value()) provider_.reset();
    }
    return outcome;
}

Result<HostStorageStats> HostCore::storage_stats(
    const WorkerBinding& binding) const {
    auto provider = current_provider(binding);
    if (!provider.ok()) {
        return Result<HostStorageStats>::failure(provider.error());
    }
    try {
        return provider.value()->storage_stats();
    } catch (...) {
        return Result<HostStorageStats>::failure(core_error(
            HostCoreError::provider_exception,
            L"HostCore::storage_stats",
            L"The provider statistics query threw an exception"));
    }
}

Result<HostProviderState> HostCore::provider_state(
    const WorkerBinding& binding) const {
    auto provider = current_provider(binding);
    if (!provider.ok()) {
        return Result<HostProviderState>::failure(provider.error());
    }
    try {
        return provider.value()->state();
    } catch (...) {
        return Result<HostProviderState>::failure(core_error(
            HostCoreError::provider_exception,
            L"HostCore::provider_state",
            L"The provider state query threw an exception"));
    }
}

namespace m3_adapter {
namespace {

template <typename T>
Result<T> mapping_failure(const wchar_t* operation, const wchar_t* message) {
    return Result<T>::failure(core_error(
        HostCoreError::adapter_invariant,
        operation,
        message,
        ErrorDomain::internal));
}

bool has_valid_failure_evidence(
    const Error& error,
    const persistence::PersistenceErrorContext& context,
    persistence::CommitDisposition disposition,
    std::uint64_t generation) noexcept {
    return error.domain == ErrorDomain::persistence
        && persistence::is_complete_error_context(context)
        && error.code == static_cast<std::uint32_t>(context.code)
        && error.native_code == context.native_code
        && persistence::operation_labels_match(
            error.operation, context.operation)
        && context.generation == generation
        && context.disposition == disposition;
}

} // namespace

Result<HostProviderState> map_state(m3::M3ProviderState state) {
    HostProviderState mapped{};
    switch (state) {
    case m3::M3ProviderState::Constructed:
        mapped = HostProviderState::constructed;
        break;
    case m3::M3ProviderState::Starting:
        mapped = HostProviderState::starting;
        break;
    case m3::M3ProviderState::Ready:
        mapped = HostProviderState::ready;
        break;
    case m3::M3ProviderState::Stopping:
        mapped = HostProviderState::stopping;
        break;
    case m3::M3ProviderState::Stopped:
        mapped = HostProviderState::stopped;
        break;
    case m3::M3ProviderState::Failed:
        mapped = HostProviderState::failed;
        break;
    default:
        return mapping_failure<HostProviderState>(
            L"m3_adapter.map_state", L"M3 returned an unknown provider state");
    }
    return Result<HostProviderState>::success(mapped);
}

Result<HostCommitDisposition> map_disposition(
    persistence::CommitDisposition disposition) {
    switch (disposition) {
    case persistence::CommitDisposition::NotCommitted:
        return Result<HostCommitDisposition>::success(
            HostCommitDisposition::not_committed);
    case persistence::CommitDisposition::CommitIndeterminate:
        return Result<HostCommitDisposition>::success(
            HostCommitDisposition::commit_indeterminate);
    case persistence::CommitDisposition::Committed:
        return Result<HostCommitDisposition>::success(
            HostCommitDisposition::committed);
    case persistence::CommitDisposition::CommittedNeedsReconciliation:
        return Result<HostCommitDisposition>::success(
            HostCommitDisposition::committed_needs_reconciliation);
    }
    return mapping_failure<HostCommitDisposition>(
        L"m3_adapter.map_disposition",
        L"M3 returned an unknown commit disposition");
}

Result<HostVolumeBarrierKind> map_barrier(
    persistence::VolumeBarrierKind barrier) {
    switch (barrier) {
    case persistence::VolumeBarrierKind::WindowsVolumeFlushed:
        return Result<HostVolumeBarrierKind>::success(
            HostVolumeBarrierKind::windows_volume_flushed);
    case persistence::VolumeBarrierKind::RawBlockTestNoFilesystemCache:
        return Result<HostVolumeBarrierKind>::success(
            HostVolumeBarrierKind::raw_block_test_no_filesystem_cache);
    }
    return mapping_failure<HostVolumeBarrierKind>(
        L"m3_adapter.map_barrier", L"M3 returned an unknown volume barrier");
}

Result<persistence::VolumeBarrierKind> map_barrier(
    HostVolumeBarrierKind barrier) {
    switch (barrier) {
    case HostVolumeBarrierKind::windows_volume_flushed:
        return Result<persistence::VolumeBarrierKind>::success(
            persistence::VolumeBarrierKind::WindowsVolumeFlushed);
    case HostVolumeBarrierKind::raw_block_test_no_filesystem_cache:
        return Result<persistence::VolumeBarrierKind>::success(
            persistence::VolumeBarrierKind::RawBlockTestNoFilesystemCache);
    }
    return mapping_failure<persistence::VolumeBarrierKind>(
        L"m3_adapter.map_barrier",
        L"Host supplied an unknown volume barrier");
}

Result<HostGeometry> map_geometry(
    const persistence::ImageGeometry& geometry) {
    if (persistence::validate_geometry(geometry)
        != persistence::GeometryStatus::Ok) {
        return mapping_failure<HostGeometry>(
            L"m3_adapter.map_geometry",
            L"M3 returned invalid image geometry");
    }
    return Result<HostGeometry>::success({
        geometry.virtual_bytes,
        geometry.logical_sector_size,
        geometry.physical_sector_size});
}

Result<HostPersistenceErrorContext> map_error_context(
    const persistence::PersistenceErrorContext& context) {
    if (!persistence::is_complete_error_context(context)) {
        return mapping_failure<HostPersistenceErrorContext>(
            L"m3_adapter.map_error_context",
            L"M3 returned incomplete persistence failure evidence");
    }
    auto disposition = map_disposition(context.disposition);
    if (!disposition.ok()) {
        return Result<HostPersistenceErrorContext>::failure(
            disposition.error());
    }
    return Result<HostPersistenceErrorContext>::success({
        context.operation,
        static_cast<std::uint32_t>(context.code),
        static_cast<std::uint8_t>(context.phase),
        context.native_code,
        context.operation_id,
        context.image_uuid.bytes,
        context.generation,
        disposition.value(),
        context.safe_context});
}

Result<HostCheckpointOutcome> map_checkpoint_outcome(
    const m3::M3CheckpointOutcome& outcome) {
    auto disposition = map_disposition(outcome.disposition);
    if (!disposition.ok()) {
        return Result<HostCheckpointOutcome>::failure(disposition.error());
    }
    if (outcome.generation == persistence::kInvalidGeneration
        || outcome.error.has_value() != outcome.error_context.has_value()) {
        return mapping_failure<HostCheckpointOutcome>(
            L"m3_adapter.map_checkpoint_outcome",
            L"M3 returned contradictory checkpoint evidence");
    }
    if (!outcome.error.has_value()) {
        if (outcome.disposition !=
            persistence::CommitDisposition::Committed) {
            return mapping_failure<HostCheckpointOutcome>(
                L"m3_adapter.map_checkpoint_outcome",
                L"A non-committed checkpoint omitted failure evidence");
        }
        return Result<HostCheckpointOutcome>::success({
            disposition.value(), outcome.generation});
    }

    const auto& error = *outcome.error;
    const auto& context = *outcome.error_context;
    if (!has_valid_failure_evidence(
            error, context, outcome.disposition, outcome.generation)) {
        return mapping_failure<HostCheckpointOutcome>(
            L"m3_adapter.map_checkpoint_outcome",
            L"Checkpoint failure evidence is incomplete or inconsistent");
    }
    switch (outcome.disposition) {
    case persistence::CommitDisposition::NotCommitted:
        if (context.code ==
            persistence::PersistenceErrorCode::CommitIndeterminate) {
            return mapping_failure<HostCheckpointOutcome>(
                L"m3_adapter.map_checkpoint_outcome",
                L"NotCommitted carries an indeterminate error code");
        }
        break;
    case persistence::CommitDisposition::CommitIndeterminate:
        if (context.code !=
            persistence::PersistenceErrorCode::CommitIndeterminate) {
            return mapping_failure<HostCheckpointOutcome>(
                L"m3_adapter.map_checkpoint_outcome",
                L"CommitIndeterminate lacks its exact error code");
        }
        break;
    case persistence::CommitDisposition::Committed:
        return mapping_failure<HostCheckpointOutcome>(
            L"m3_adapter.map_checkpoint_outcome",
            L"A committed checkpoint cannot carry failure evidence");
    case persistence::CommitDisposition::CommittedNeedsReconciliation:
        if (context.code == persistence::PersistenceErrorCode::Cancelled
            || context.code ==
                persistence::PersistenceErrorCode::DeadlineExceeded) {
            return mapping_failure<HostCheckpointOutcome>(
                L"m3_adapter.map_checkpoint_outcome",
                L"Committed data cannot become cancelled or timed out");
        }
        break;
    default:
        return mapping_failure<HostCheckpointOutcome>(
            L"m3_adapter.map_checkpoint_outcome",
            L"Checkpoint disposition is unknown");
    }
    auto mapped_context = map_error_context(context);
    if (!mapped_context.ok()) {
        return Result<HostCheckpointOutcome>::failure(mapped_context.error());
    }
    return Result<HostCheckpointOutcome>::success({
        disposition.value(),
        outcome.generation,
        error,
        mapped_context.move_value()});
}

Result<HostCompactionBoundaryEvidence> map_compaction_boundary(
    const m3::M3CompactionBoundaryEvidence& evidence) {
    if (evidence.operation_id == 0 || evidence.image_uuid.is_nil() ||
        evidence.source_generation >= persistence::kInvalidGeneration - 1 ||
        evidence.candidate_generation != evidence.source_generation + 1 ||
        (!evidence.cut_dirty_epoch_known
            && evidence.cut_dirty_epoch != 0) ||
        evidence.required_peak_free_bytes == 0 ||
        evidence.available_free_bytes <
            evidence.required_peak_free_bytes) {
        return mapping_failure<HostCompactionBoundaryEvidence>(
            L"m3_adapter.map_compaction_boundary",
            L"M3 returned invalid compaction boundary evidence");
    }
    return Result<HostCompactionBoundaryEvidence>::success({
        evidence.operation_id,
        evidence.image_uuid.bytes,
        evidence.source_generation,
        evidence.candidate_generation,
        evidence.source_base_bytes,
        evidence.source_journal_bytes,
        evidence.required_peak_free_bytes,
        evidence.available_free_bytes,
        evidence.cut_dirty_epoch,
        evidence.cut_dirty_epoch_known});
}

Result<m3::M3CompactionBoundaryEvidence> map_compaction_boundary(
    const HostCompactionBoundaryEvidence& evidence) {
    persistence::ImageUuid image_uuid{evidence.image_uuid};
    if (evidence.native_operation_id == 0 || image_uuid.is_nil()
        || evidence.source_generation
            >= persistence::kInvalidGeneration - 1
        || evidence.candidate_generation != evidence.source_generation + 1
        || (!evidence.cut_dirty_epoch_known
            && evidence.cut_dirty_epoch != 0)
        || evidence.source_base_bytes == 0
        || evidence.source_journal_bytes == 0
        || evidence.required_peak_free_bytes == 0
        || evidence.available_free_bytes
            < evidence.required_peak_free_bytes) {
        return mapping_failure<m3::M3CompactionBoundaryEvidence>(
            L"m3_adapter.map_compaction_boundary",
            L"Host returned invalid compaction recovery evidence");
    }
    return Result<m3::M3CompactionBoundaryEvidence>::success({
        evidence.native_operation_id,
        image_uuid,
        evidence.source_generation,
        evidence.candidate_generation,
        evidence.source_base_bytes,
        evidence.source_journal_bytes,
        evidence.required_peak_free_bytes,
        evidence.available_free_bytes,
        evidence.cut_dirty_epoch,
        evidence.cut_dirty_epoch_known});
}

Result<HostCompactionOutcome> map_compaction_outcome(
    const m3::M3CompactionOutcome& outcome) {
    auto checkpoint_like = map_checkpoint_outcome({
        outcome.disposition,
        outcome.generation,
        outcome.error,
        outcome.error_context});
    if (!checkpoint_like.ok()) {
        return Result<HostCompactionOutcome>::failure(
            checkpoint_like.error());
    }
    const auto& source = outcome.metrics;
    const bool boundary_present = outcome.boundary.operation_id != 0;
    if (!boundary_present) {
        if (outcome.disposition !=
                persistence::CommitDisposition::NotCommitted
            || outcome.boundary != m3::M3CompactionBoundaryEvidence{}
            || source != m3::M3CompactionMetrics{}) {
            return mapping_failure<HostCompactionOutcome>(
                L"m3_adapter.map_compaction_outcome",
                L"A preflight compaction failure returned partial boundary state");
        }
        return Result<HostCompactionOutcome>::success({
            checkpoint_like.value().disposition,
            checkpoint_like.value().generation,
            {},
            {},
            checkpoint_like.value().error,
            checkpoint_like.value().error_context});
    }
    const bool low_space_proof =
        outcome.boundary.required_peak_free_bytes != 0
        && outcome.boundary.available_free_bytes
            < outcome.boundary.required_peak_free_bytes;
    Result<HostCompactionBoundaryEvidence> boundary =
        low_space_proof
        ? Result<HostCompactionBoundaryEvidence>::success({
            outcome.boundary.operation_id,
            outcome.boundary.image_uuid.bytes,
            outcome.boundary.source_generation,
            outcome.boundary.candidate_generation,
            outcome.boundary.source_base_bytes,
            outcome.boundary.source_journal_bytes,
            outcome.boundary.required_peak_free_bytes,
            outcome.boundary.available_free_bytes,
            outcome.boundary.cut_dirty_epoch,
            outcome.boundary.cut_dirty_epoch_known})
        : map_compaction_boundary(outcome.boundary);
    if (!boundary.ok()) {
        return Result<HostCompactionOutcome>::failure(boundary.error());
    }
    if (low_space_proof
        && (outcome.disposition
                != persistence::CommitDisposition::NotCommitted
            || !outcome.error_context.has_value()
            || outcome.error_context->code
                != persistence::PersistenceErrorCode::DiskFull
            || outcome.boundary.image_uuid.is_nil()
            || outcome.boundary.source_generation
                >= persistence::kInvalidGeneration - 1
            || outcome.boundary.candidate_generation
                != outcome.boundary.source_generation + 1)) {
        return mapping_failure<HostCompactionOutcome>(
            L"m3_adapter.map_compaction_outcome",
            L"Insufficient compaction space lacked an exact DiskFull proof");
    }
    const bool committed = outcome.disposition
        == persistence::CommitDisposition::Committed;
    const bool generation_valid = outcome.generation
            == outcome.boundary.candidate_generation
        || (outcome.disposition
                == persistence::CommitDisposition::NotCommitted
            && outcome.generation
                == outcome.boundary.source_generation);
    if (!generation_valid
        || source.source_generation
            != outcome.boundary.source_generation
        || source.source_base_bytes
            != outcome.boundary.source_base_bytes
        || source.source_journal_bytes
            != outcome.boundary.source_journal_bytes
        || source.required_peak_free_bytes
            != outcome.boundary.required_peak_free_bytes
        || source.available_free_bytes
            != outcome.boundary.available_free_bytes
        || (committed
            && (source.committed_generation != outcome.generation
                || !outcome.boundary.cut_dirty_epoch_known
                || source.candidate_base_bytes == 0
                || source.candidate_journal_bytes == 0))
        || (!committed
            && source.committed_generation != 0
            && source.committed_generation
                != outcome.boundary.candidate_generation)) {
        return mapping_failure<HostCompactionOutcome>(
            L"m3_adapter.map_compaction_outcome",
            L"M3 returned contradictory compaction metrics");
    }
    HostCompactionMetrics metrics{
        source.source_generation,
        source.committed_generation,
        source.source_base_bytes,
        source.source_journal_bytes,
        source.candidate_base_bytes,
        source.candidate_journal_bytes,
        source.required_peak_free_bytes,
        source.available_free_bytes,
        source.live_extent_count,
        source.candidate_payloads,
        source.snapshot_records,
        source.data_records,
        source.zero_records,
        source.discard_records,
        source.index_nodes,
        source.newer_dirty_preserved,
        source.retained_candidate_count,
        source.deleted_candidate_count};
    return Result<HostCompactionOutcome>::success({
        checkpoint_like.value().disposition,
        checkpoint_like.value().generation,
        boundary.move_value(),
        metrics,
        checkpoint_like.value().error,
        checkpoint_like.value().error_context});
}

Result<HostStopOutcome> map_stop_outcome(
    const m3::M3StopOutcome& outcome) {
    auto disposition = map_disposition(outcome.disposition);
    if (!disposition.ok()) {
        return Result<HostStopOutcome>::failure(disposition.error());
    }
    if (outcome.generation == persistence::kInvalidGeneration
        || outcome.error.has_value() != outcome.error_context.has_value()
        || (outcome.checkpoint_completed
            && outcome.disposition !=
                persistence::CommitDisposition::Committed)) {
        return mapping_failure<HostStopOutcome>(
            L"m3_adapter.map_stop_outcome",
            L"M3 returned contradictory stop evidence");
    }
    std::optional<HostPersistenceErrorContext> mapped_context;
    if (outcome.error.has_value()) {
        const auto& context = *outcome.error_context;
        if (!persistence::is_complete_error_context(context)
            || context.disposition != outcome.disposition
            || context.generation != outcome.generation) {
            return mapping_failure<HostStopOutcome>(
                L"m3_adapter.map_stop_outcome",
                L"Stop failure evidence is incomplete or inconsistent");
        }
        auto converted = map_error_context(context);
        if (!converted.ok()) {
            return Result<HostStopOutcome>::failure(converted.error());
        }
        mapped_context = converted.move_value();
    } else if (!outcome.transport_stopped || !outcome.store_closed
               || !outcome.session_closed || !outcome.writer_closed) {
        return mapping_failure<HostStopOutcome>(
            L"m3_adapter.map_stop_outcome",
            L"Incomplete stop omitted failure evidence");
    }

    return Result<HostStopOutcome>::success({
        disposition.value(),
        outcome.generation,
        outcome.transport_stopped,
        outcome.checkpoint_completed,
        outcome.store_closed,
        outcome.session_closed,
        outcome.writer_closed,
        outcome.error,
        std::move(mapped_context)});
}

Result<HostStorageStats> map_storage_stats(
    const DynamicBlockStoreStats& source) {
    std::optional<HostSnapshotState> snapshot_state;
    if (source.snapshot_active_state.has_value()) {
        switch (*source.snapshot_active_state) {
        case ConsistentSnapshotState::Active:
            snapshot_state = HostSnapshotState::active;
            break;
        case ConsistentSnapshotState::Aborted:
            snapshot_state = HostSnapshotState::aborted;
            break;
        case ConsistentSnapshotState::FullyScanned:
            snapshot_state = HostSnapshotState::fully_scanned;
            break;
        default:
            return mapping_failure<HostStorageStats>(
                L"m3_adapter.map_storage_stats",
                L"M3 returned an unknown snapshot state");
        }
    }
    HostStorageStats mapped{};
    mapped.logical_size_bytes = source.logical_size_bytes;
    mapped.reserved_bytes = source.reserved_bytes;
    mapped.committed_bytes = source.committed_bytes;
    mapped.resident_clean_blocks = source.resident_clean_blocks;
    mapped.resident_dirty_blocks = source.resident_dirty_blocks;
    mapped.zero_clean_blocks = source.zero_clean_blocks;
    mapped.zero_dirty_blocks = source.zero_dirty_blocks;
    mapped.loading_blocks = source.loading_blocks;
    mapped.evicted_blocks_total = source.evicted_blocks_total;
    mapped.backing_reads_total = source.backing_reads_total;
    mapped.backing_read_bytes_total = source.backing_read_bytes_total;
    mapped.uncached_reads_total = source.uncached_reads_total;
    mapped.unmap_bytes_total = source.unmap_bytes_total;
    mapped.zero_bytes_total = source.zero_bytes_total;
    mapped.hard_limit_rejections = source.hard_limit_rejections;
    mapped.commit_failures = source.commit_failures;
    mapped.decommit_failures = source.decommit_failures;
    mapped.synchronous_pressure_passes_total =
        source.synchronous_pressure_passes_total;
    mapped.synchronous_pressure_waits_total =
        source.synchronous_pressure_waits_total;
    mapped.synchronous_pressure_no_progress_total =
        source.synchronous_pressure_no_progress_total;
    mapped.synchronous_pressure_cancellations_total =
        source.synchronous_pressure_cancellations_total;
    mapped.synchronous_pressure_deadlines_total =
        source.synchronous_pressure_deadlines_total;
    mapped.commit_reservation_rollbacks_total =
        source.commit_reservation_rollbacks_total;
    mapped.snapshot_records_total = source.snapshot_records_total;
    mapped.snapshot_version_conflicts = source.snapshot_version_conflicts;
    mapped.backing_generation = source.backing_generation;
    mapped.backing_view_installs_total = source.backing_view_installs_total;
    mapped.backing_generation_conflicts =
        source.backing_generation_conflicts;
    mapped.stale_backing_load_retries = source.stale_backing_load_retries;
    mapped.snapshot_active_id = source.snapshot_active_id;
    mapped.snapshot_active_state = snapshot_state;
    mapped.snapshot_cow_current_bytes = source.snapshot_cow_current_bytes;
    mapped.snapshot_cow_peak_bytes = source.snapshot_cow_peak_bytes;
    mapped.snapshot_cow_records = source.snapshot_cow_records;
    mapped.snapshot_blocked_writers = source.snapshot_blocked_writers;
    mapped.snapshot_writer_stalls_total =
        source.snapshot_writer_stalls_total;
    mapped.snapshot_auto_aborts_total = source.snapshot_auto_aborts_total;
    mapped.snapshot_consistent_batches_total =
        source.snapshot_consistent_batches_total;
    mapped.snapshot_consistent_records_total =
        source.snapshot_consistent_records_total;
    return Result<HostStorageStats>::success(std::move(mapped));
}

} // namespace m3_adapter

namespace {

class M3HostProviderAdapter final : public IHostProvider {
public:
    explicit M3HostProviderAdapter(std::unique_ptr<m3::M3Provider> provider)
        : provider_(std::move(provider)) {}

    Result<void> start(const OperationContext& context) override {
        return provider_->start(context);
    }

    HostCheckpointOutcome checkpoint(
        const OperationContext& context,
        HostVolumeBarrierKind barrier) override {
        auto mapped_barrier = m3_adapter::map_barrier(barrier);
        if (!mapped_barrier.ok()) {
            return failed_outcome<HostCheckpointOutcome>(
                mapped_barrier.error());
        }
        auto mapped = m3_adapter::map_checkpoint_outcome(
            provider_->checkpoint(context, mapped_barrier.value()));
        if (!mapped.ok()) {
            return failed_outcome<HostCheckpointOutcome>(mapped.error());
        }
        return mapped.move_value();
    }

    HostCompactionOutcome compact(
        const OperationContext& context,
        const HostCompactionRequest request,
        const HostCompactionBoundaryHandler& boundary,
        const HostCompactionCutHandler& cut) override {
        auto mapped_barrier = m3_adapter::map_barrier(request.barrier);
        if (!mapped_barrier.ok()) {
            return failed_outcome<HostCompactionOutcome>(
                mapped_barrier.error());
        }
        const auto outcome = provider_->compact(
            context,
            {mapped_barrier.value(), request.native_operation_id},
            [&](const m3::M3CompactionBoundaryEvidence& evidence)
                -> Result<m3::M3CompactionBoundaryDecision> {
                auto mapped =
                    m3_adapter::map_compaction_boundary(evidence);
                if (!mapped.ok()) {
                    return Result<m3::M3CompactionBoundaryDecision>::failure(
                        mapped.error());
                }
                auto decision = boundary(mapped.value());
                if (!decision.ok()) {
                    return Result<m3::M3CompactionBoundaryDecision>::failure(
                        decision.error());
                }
                if (decision.value().evidence != mapped.value()) {
                    return Result<m3::M3CompactionBoundaryDecision>::failure(
                        core_error(
                            HostCoreError::adapter_invariant,
                            L"M3HostProviderAdapter::compact",
                            L"The host compaction boundary proof changed identity",
                            ErrorDomain::protocol));
                }
                return Result<m3::M3CompactionBoundaryDecision>::success({
                    decision.value().continue_compaction,
                    decision.value().durable_operation_revision,
                    decision.value().durable_profile_revision,
                    evidence});
            },
            [&](const m3::M3CompactionBoundaryEvidence& evidence)
                -> Result<m3::M3CompactionBoundaryDecision> {
                auto mapped =
                    m3_adapter::map_compaction_boundary(evidence);
                if (!mapped.ok()) {
                    return Result<m3::M3CompactionBoundaryDecision>::failure(
                        mapped.error());
                }
                auto decision = cut(mapped.value());
                if (!decision.ok()) {
                    return Result<m3::M3CompactionBoundaryDecision>::failure(
                        decision.error());
                }
                if (decision.value().evidence != mapped.value()) {
                    return Result<m3::M3CompactionBoundaryDecision>::failure(
                        core_error(
                            HostCoreError::adapter_invariant,
                            L"M3HostProviderAdapter::compact",
                            L"The host compaction cut proof changed identity",
                            ErrorDomain::protocol));
                }
                return Result<m3::M3CompactionBoundaryDecision>::success({
                    decision.value().continue_compaction,
                    decision.value().durable_operation_revision,
                    decision.value().durable_profile_revision,
                    evidence});
            });
        auto mapped = m3_adapter::map_compaction_outcome(outcome);
        if (!mapped.ok()) {
            return failed_outcome<HostCompactionOutcome>(mapped.error());
        }
        return mapped.move_value();
    }

    HostCompactionOutcome reconcile_compaction(
        const OperationContext& context,
        const HostCompactionBoundaryEvidence& boundary) override {
        auto mapped_boundary =
            m3_adapter::map_compaction_boundary(boundary);
        if (!mapped_boundary.ok()) {
            return failed_outcome<HostCompactionOutcome>(
                mapped_boundary.error());
        }
        auto mapped = m3_adapter::map_compaction_outcome(
            provider_->reconcile_compaction(
                context, mapped_boundary.value()));
        if (!mapped.ok()) {
            return failed_outcome<HostCompactionOutcome>(mapped.error());
        }
        return mapped.move_value();
    }

    HostStopOutcome stop(
        const OperationContext& context,
        HostStopRequest request) override {
        m3::M3StopRequest mapped_request{};
        mapped_request.checkpoint = request.checkpoint;
        if (request.barrier.has_value()) {
            auto barrier = m3_adapter::map_barrier(*request.barrier);
            if (!barrier.ok()) {
                return failed_outcome<HostStopOutcome>(barrier.error());
            }
            mapped_request.barrier = barrier.value();
        }
        auto mapped = m3_adapter::map_stop_outcome(
            provider_->stop(context, mapped_request));
        if (!mapped.ok()) {
            return failed_outcome<HostStopOutcome>(mapped.error());
        }
        return mapped.move_value();
    }

    Result<HostProviderState> state() const override {
        return m3_adapter::map_state(provider_->state());
    }

    Result<std::uint64_t> generation() const override {
        return Result<std::uint64_t>::success(provider_->generation());
    }

    Result<HostGeometry> geometry() const override {
        return m3_adapter::map_geometry(provider_->geometry());
    }

    Result<HostStorageStats> storage_stats() const override {
        return m3_adapter::map_storage_stats(provider_->storage_stats());
    }

private:
    std::unique_ptr<m3::M3Provider> provider_;
};

class M3HostProviderFactory final : public IHostProviderFactory {
public:
    explicit M3HostProviderFactory(m3::M3ProviderConfig config)
        : config_(std::move(config)) {}

    Result<std::shared_ptr<IHostProvider>> open(
        const OperationContext& context) override {
        auto opened = m3::M3Provider::open(context, config_);
        if (!opened.ok()) {
            return Result<std::shared_ptr<IHostProvider>>::failure(
                opened.error());
        }
        return Result<std::shared_ptr<IHostProvider>>::success(
            adapt_m3_provider(opened.move_value()));
    }

private:
    m3::M3ProviderConfig config_;
};

} // namespace

std::shared_ptr<IHostProvider> adapt_m3_provider(
    std::unique_ptr<m3::M3Provider> provider) {
    if (!provider) return {};
    return std::make_shared<M3HostProviderAdapter>(std::move(provider));
}

std::shared_ptr<IHostProviderFactory> make_m3_host_provider_factory(
    m3::M3ProviderConfig config) {
    return std::make_shared<M3HostProviderFactory>(std::move(config));
}

MutationCompletion::~MutationCompletion() noexcept {
    core_->end_mutation();
}

} // namespace myrd::host
