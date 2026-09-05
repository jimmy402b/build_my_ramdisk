#include "compaction_coordinator.h"

#include "src/persistence/image_paths.h"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <span>
#include <vector>

namespace myrd::persistence {
namespace {

template <class T>
Result<T> compaction_failure(
    const PersistenceErrorCode code,
    const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"CompactionCoordinator::begin",
        message,
        0});
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<void> run_phase_hook(
    const CompactionPhaseHook* hook,
    const CompactionPhase phase) {
    if (hook == nullptr || !*hook) return Result<void>::success();
    try {
        return (*hook)(phase);
    } catch (...) {
        return compaction_failure<void>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"compaction phase hook threw an exception");
    }
}
#endif

PersistenceErrorCode compaction_error_code(const Error& error) noexcept {
    if (error.domain == ErrorDomain::persistence) {
        const auto decoded = decode_persistence_error_code(error.code);
        if (decoded.has_value()) return *decoded;
    }
    if (error.code == static_cast<std::uint32_t>(
            OperationErrorCode::cancelled)) {
        return PersistenceErrorCode::Cancelled;
    }
    if (error.code == static_cast<std::uint32_t>(
            OperationErrorCode::timed_out)) {
        return PersistenceErrorCode::DeadlineExceeded;
    }
    return PersistenceErrorCode::IoError;
}

// Compare a/b with c/d exactly without cross multiplication. Each reciprocal
// step reverses ordering, and Euclidean reduction guarantees termination.
int compare_positive_fractions(
    std::uint64_t a,
    std::uint64_t b,
    std::uint64_t c,
    std::uint64_t d) noexcept {
    bool reversed = false;
    for (;;) {
        const auto lhs_quotient = a / b;
        const auto rhs_quotient = c / d;
        if (lhs_quotient != rhs_quotient) {
            const int comparison = lhs_quotient < rhs_quotient ? -1 : 1;
            return reversed ? -comparison : comparison;
        }

        const auto lhs_remainder = a % b;
        const auto rhs_remainder = c % d;
        if (lhs_remainder == 0 || rhs_remainder == 0) {
            int comparison = 0;
            if (lhs_remainder == 0 && rhs_remainder != 0) {
                comparison = -1;
            } else if (lhs_remainder != 0 && rhs_remainder == 0) {
                comparison = 1;
            }
            return reversed ? -comparison : comparison;
        }

        a = b;
        b = lhs_remainder;
        c = d;
        d = rhs_remainder;
        reversed = !reversed;
    }
}

bool same_geometry(
    const ImageGeometry& left,
    const ImageGeometry& right) noexcept {
    return left.virtual_bytes == right.virtual_bytes &&
        left.logical_sector_size == right.logical_sector_size &&
        left.physical_sector_size == right.physical_sector_size;
}

bool active_slot_matches_source(
    const ManifestSlot& slot,
    const CompositeBackingIdentity& source,
    const BackingGeneration captured_generation) noexcept {
    return slot.image_uuid == source.image_uuid &&
        same_geometry(slot.geometry, source.geometry) &&
        slot.generation.checkpoint_generation == captured_generation &&
        source.checkpoint_generation == captured_generation &&
        slot.generation.base_generation == source.base_generation &&
        slot.base_identity == source.base_vhdx_identity &&
        slot.base_expected_length == source.base_expected_length &&
        slot.generation.journal.epoch == source.journal_epoch &&
        slot.generation.journal.committed_length ==
            source.journal_committed_length &&
        slot.generation.journal.commit == source.commit &&
        slot.generation.journal.root == source.root &&
        source.journal_root_generation == captured_generation &&
        slot.journal_identity == source.journal_identity &&
        slot.feature_flags == source.feature_flags;
}

template <std::size_t N>
bool all_zero(const std::array<std::byte, N>& bytes) noexcept {
    return std::all_of(bytes.begin(), bytes.end(), [](const std::byte value) {
        return value == std::byte{};
    });
}

class CompactionPayloadSource final : public IVhdxPayloadSource {
public:
    struct PayloadExtentRange final {
        std::uint64_t payload_index{};
        std::size_t first_extent{};
        std::size_t one_past_extent{};
    };

    static Result<std::unique_ptr<CompactionPayloadSource>> create(
        const OperationContext& context,
        BackingView captured_view,
        const CompositeBackingIdentity& identity,
        const LiveExtentEnumeratorOptions options,
        const std::uint64_t max_live_extents,
        const std::uint64_t max_candidate_payloads) {
        if (max_live_extents == 0 || max_candidate_payloads == 0 ||
            options.max_extent_bytes == 0 ||
            options.max_extent_bytes > kVhdxDataBlockBytes) {
            return compaction_failure<
                std::unique_ptr<CompactionPayloadSource>>(
                    PersistenceErrorCode::SnapshotResourceExhausted,
                    L"compaction extent and payload limits are invalid");
        }
        auto enumerator = LiveExtentEnumerator::create(
            std::move(captured_view), identity, options);
        if (!enumerator.ok()) {
            return Result<std::unique_ptr<CompactionPayloadSource>>::failure(
                enumerator.error());
        }
        auto result = std::unique_ptr<CompactionPayloadSource>(
            new CompactionPayloadSource(enumerator.move_value()));
        auto enumerated = result->enumerator_->enumerate(
            context,
            [&](const LiveDataExtent& extent) -> Result<void> {
                if (result->extents_.size() >= max_live_extents) {
                    return compaction_failure<void>(
                        PersistenceErrorCode::SnapshotResourceExhausted,
                        L"compaction live-extent limit was exceeded");
                }
                if (extent.length == 0 ||
                    extent.length > options.max_extent_bytes ||
                    extent.logical_offset >= identity.geometry.virtual_bytes ||
                    extent.length > identity.geometry.virtual_bytes -
                        extent.logical_offset) {
                    return compaction_failure<void>(
                        PersistenceErrorCode::FatalInternalInvariant,
                        L"live extent escaped the captured image geometry");
                }
                const auto extent_index = result->extents_.size();
                result->extents_.push_back(extent);
                const auto first_payload =
                    extent.logical_offset / kVhdxDataBlockBytes;
                const auto final_byte =
                    extent.logical_offset + extent.length - 1;
                const auto last_payload =
                    final_byte / kVhdxDataBlockBytes;
                for (auto payload = first_payload;; ++payload) {
                    if (result->ranges_.empty() ||
                        result->ranges_.back().payload_index != payload) {
                        if (result->ranges_.size() >=
                            max_candidate_payloads) {
                            return compaction_failure<void>(
                                PersistenceErrorCode::SnapshotResourceExhausted,
                                L"compaction candidate-payload limit was exceeded");
                        }
                        result->ranges_.push_back(
                            {payload, extent_index, extent_index + 1});
                        result->candidate_payloads_.push_back(payload);
                    } else {
                        result->ranges_.back().one_past_extent =
                            extent_index + 1;
                    }
                    if (payload == last_payload) break;
                }
                return Result<void>::success();
            });
        if (!enumerated.ok()) {
            return Result<std::unique_ptr<CompactionPayloadSource>>::failure(
                enumerated.error());
        }
        result->metrics_ = enumerated.value();
        return Result<std::unique_ptr<CompactionPayloadSource>>::success(
            std::move(result));
    }

    Result<void> read_payload(
        const OperationContext& context,
        const std::uint64_t payload_index,
        const std::span<std::byte> destination) const override {
        if (destination.size() != kVhdxDataBlockBytes) {
            return compaction_failure<void>(
                PersistenceErrorCode::InvalidFormat,
                L"VHDX writer requested a noncanonical payload size");
        }
        const auto found = std::lower_bound(
            ranges_.begin(), ranges_.end(), payload_index,
            [](const PayloadExtentRange& range, const std::uint64_t value) {
                return range.payload_index < value;
            });
        if (found == ranges_.end() || found->payload_index != payload_index) {
            return compaction_failure<void>(
                PersistenceErrorCode::FatalInternalInvariant,
                L"VHDX writer requested an unplanned compaction payload");
        }
        std::fill(destination.begin(), destination.end(), std::byte{});
        const auto payload_offset = payload_index *
            static_cast<std::uint64_t>(kVhdxDataBlockBytes);
        const auto payload_end = payload_offset + kVhdxDataBlockBytes;
        for (auto index = found->first_extent;
             index < found->one_past_extent; ++index) {
            const auto& extent = extents_[index];
            std::vector<std::byte> scratch(
                static_cast<std::size_t>(extent.length));
            auto loaded = enumerator_->read_verified_extent(
                context, extent, scratch);
            if (!loaded.ok()) return loaded;
            const auto extent_end = extent.logical_offset + extent.length;
            const auto overlap_begin = (std::max)(
                extent.logical_offset, payload_offset);
            const auto overlap_end = (std::min)(extent_end, payload_end);
            if (overlap_begin >= overlap_end) {
                return compaction_failure<void>(
                    PersistenceErrorCode::FatalInternalInvariant,
                    L"compaction payload index does not overlap its extent");
            }
            const auto source_begin = static_cast<std::size_t>(
                overlap_begin - extent.logical_offset);
            const auto destination_begin = static_cast<std::size_t>(
                overlap_begin - payload_offset);
            const auto length = static_cast<std::size_t>(
                overlap_end - overlap_begin);
            std::copy_n(
                scratch.begin() + static_cast<std::ptrdiff_t>(source_begin),
                length,
                destination.begin() +
                    static_cast<std::ptrdiff_t>(destination_begin));
        }
        return check_operation_context(context);
    }

    [[nodiscard]] std::span<const std::uint64_t>
    candidate_payloads() const noexcept {
        return candidate_payloads_;
    }

    [[nodiscard]] const LiveExtentEnumerationMetrics& metrics() const noexcept {
        return metrics_;
    }

private:
    explicit CompactionPayloadSource(
        std::unique_ptr<LiveExtentEnumerator> enumerator) noexcept
        : enumerator_(std::move(enumerator)) {}

    std::unique_ptr<LiveExtentEnumerator> enumerator_;
    std::vector<LiveDataExtent> extents_;
    std::vector<PayloadExtentRange> ranges_;
    std::vector<std::uint64_t> candidate_payloads_;
    LiveExtentEnumerationMetrics metrics_{};
};

Result<void> validate_artifact_request(
    const CompactionArtifactBuildRequest& request,
    const CompositeBackingIdentity& source,
    const BackingGeneration captured_generation) {
    if (request.writer_lock == nullptr ||
        !request.offline_vhdx_validator || request.operation_id == 0 ||
        request.journal_epoch == 0 ||
        request.journal_epoch == kInvalidGeneration ||
        source.journal_epoch >= kInvalidGeneration - 1 ||
        request.journal_epoch != source.journal_epoch + 1 ||
        request.limits.max_live_extents == 0 ||
        all_zero(request.base_identity.file_identity.file_write_guid) ||
        all_zero(request.base_identity.file_identity.data_write_guid) ||
        all_zero(request.base_identity.virtual_disk_id) ||
        request.base_identity.file_identity == source.base_vhdx_identity ||
        request.base_identity.virtual_disk_id != source.virtual_disk_id ||
        all_zero(request.journal_identity.journal_uuid) ||
        request.journal_identity == source.journal_identity ||
        !active_slot_matches_source(
            request.active_slot, source, captured_generation) ||
        !validate_manifest_slot_semantics(request.active_slot).ok()) {
        return compaction_failure<void>(
            PersistenceErrorCode::InvalidFormat,
            L"compaction artifact request does not match captured generation");
    }
    std::array<ImageComponent, 3> components{};
    std::size_t component_index = 0;
    for (const auto name : {
             std::string_view{request.base_temporary_name},
             std::string_view{request.base_relative_name},
             std::string_view{request.journal_relative_name}}) {
        auto valid = validate_image_component(name);
        if (!valid.ok()) return Result<void>::failure(valid.error());
        components[component_index++] = valid.move_value();
    }
    const auto aliases = [](const ImageComponent& left,
                            const ImageComponent& right) {
        return CompareStringOrdinal(
            left.wide_name.data(), static_cast<int>(left.wide_name.size()),
            right.wide_name.data(), static_cast<int>(right.wide_name.size()),
            TRUE) == CSTR_EQUAL;
    };
    if (aliases(components[0], components[1]) ||
        aliases(components[0], components[2]) ||
        aliases(components[1], components[2])) {
        return compaction_failure<void>(
            PersistenceErrorCode::InvalidFormat,
            L"compaction artifact names must be unique");
    }
    return request.writer_lock->verify();
}

struct CreatedJournalEpoch final {
    std::shared_ptr<IPersistenceFile> file;
    JournalHeader header{};
    WindowsFileIdentity windows_identity{};
};

Result<CreatedJournalEpoch> create_journal_epoch(
    const OperationContext& context,
    BundleWriterLock& writer,
    const CompactionArtifactBuildRequest& request,
    const CompositeBackingIdentity& source,
    const VhdxPublishedBase& base,
    const std::function<Result<void>(CompactionPhase)>& emit) {
    auto capability = create_bundle_file_for_mutation(
        writer, request.journal_relative_name);
    if (!capability.ok()) {
        return Result<CreatedJournalEpoch>::failure(capability.error());
    }
    auto opened = open_validated_persistence_file(capability.move_value());
    if (!opened.ok()) {
        return Result<CreatedJournalEpoch>::failure(opened.error());
    }
    std::shared_ptr<IPersistenceFile> file(opened.move_value());
    auto phase = emit(CompactionPhase::JournalCreateAfter);
    if (!phase.ok()) {
        return Result<CreatedJournalEpoch>::failure(phase.error());
    }

    JournalHeader header{};
    header.image_uuid = source.image_uuid;
    header.journal_identity = request.journal_identity;
    header.epoch = request.journal_epoch;
    header.geometry = source.geometry;
    header.base_checkpoint_generation = source.checkpoint_generation;
    header.predecessor_kind = JournalPredecessorKind::Checkpoint;
    header.predecessor_epoch = source.journal_epoch;
    header.predecessor_journal_identity = source.journal_identity;
    header.predecessor_digest = source.commit.chain_digest;
    auto geometry_digest = compute_geometry_digest(source.geometry);
    auto base_digest = compute_base_identity_digest(
        request.base_identity.file_identity);
    if (!geometry_digest.ok()) {
        return Result<CreatedJournalEpoch>::failure(geometry_digest.error());
    }
    if (!base_digest.ok()) {
        return Result<CreatedJournalEpoch>::failure(base_digest.error());
    }
    header.geometry_digest = geometry_digest.value();
    header.base_identity_digest = base_digest.value();
    header.feature_flags = source.feature_flags;
    auto encoded = encode_journal_header(header);
    if (!encoded.ok()) {
        return Result<CreatedJournalEpoch>::failure(encoded.error());
    }
    auto truncated = file->truncate(context, kJournalAlignment);
    if (!truncated.ok()) {
        return Result<CreatedJournalEpoch>::failure(truncated.error());
    }
    auto written = file->write_exact(context, 0, encoded.value());
    if (!written.ok()) {
        return Result<CreatedJournalEpoch>::failure(written.error());
    }
    phase = emit(CompactionPhase::JournalFlushBefore);
    if (!phase.ok()) {
        return Result<CreatedJournalEpoch>::failure(phase.error());
    }
    auto flushed = file->flush(context);
    if (!flushed.ok()) {
        return Result<CreatedJournalEpoch>::failure(flushed.error());
    }
    phase = emit(CompactionPhase::JournalFlushAfter);
    if (!phase.ok()) {
        return Result<CreatedJournalEpoch>::failure(phase.error());
    }
    JournalHeaderBytes readback{};
    auto read = file->read_exact(context, 0, readback);
    if (!read.ok()) {
        return Result<CreatedJournalEpoch>::failure(read.error());
    }
    if (readback != encoded.value()) {
        return compaction_failure<CreatedJournalEpoch>(
            PersistenceErrorCode::VerificationFailed,
            L"new journal header readback differs after flush");
    }
    auto still_live = check_operation_context(context);
    if (!still_live.ok()) {
        return Result<CreatedJournalEpoch>::failure(still_live.error());
    }
    auto* validated = dynamic_cast<ValidatedPersistenceFile*>(file.get());
    if (validated == nullptr) {
        return compaction_failure<CreatedJournalEpoch>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"new journal lost its writer-authorized file capability");
    }
    auto identity = validated->observed_identity();
    if (identity.size != kJournalAlignment ||
        base.layout().file_identity() !=
            request.base_identity.file_identity) {
        return compaction_failure<CreatedJournalEpoch>(
            PersistenceErrorCode::VerificationFailed,
            L"new compaction artifact identity differs after readback");
    }
    const auto journal_path = validated->observed_final_path();
    if (journal_path.empty()) {
        return compaction_failure<CreatedJournalEpoch>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"new journal lost its validated final path");
    }
    auto inspection = open_bundle_file_for_inspection(
        std::filesystem::path(journal_path).parent_path(),
        request.journal_relative_name, identity);
    if (!inspection.ok()) {
        return Result<CreatedJournalEpoch>::failure(inspection.error());
    }
    auto inspection_file = open_validated_persistence_file(
        inspection.move_value());
    if (!inspection_file.ok()) {
        return Result<CreatedJournalEpoch>::failure(
            inspection_file.error());
    }
    JournalHeaderBytes reopened_bytes{};
    auto reopened_read = inspection_file.value()->read_exact(
        context, 0, reopened_bytes);
    if (!reopened_read.ok()) {
        return Result<CreatedJournalEpoch>::failure(reopened_read.error());
    }
    auto reopened_header = decode_journal_header(reopened_bytes);
    if (!reopened_header.ok() || reopened_header.value() != header) {
        return compaction_failure<CreatedJournalEpoch>(
            PersistenceErrorCode::VerificationFailed,
            L"identity-reopened journal header failed format validation");
    }
    auto inspection_closed = inspection_file.value()->close();
    if (!inspection_closed.ok()) {
        return Result<CreatedJournalEpoch>::failure(
            inspection_closed.error());
    }
    return Result<CreatedJournalEpoch>::success(
        {std::move(file), std::move(header), std::move(identity)});
}

}  // namespace

PreparedCompactionOperation::PreparedCompactionOperation(
    PublishOperationLease lease,
    BackingView captured_view) noexcept
    : lease_(std::move(lease)),
      captured_view_(std::move(captured_view)) {}

PreparedCompactionOperation::PreparedCompactionOperation(
    PreparedCompactionOperation&& other) noexcept
    : lease_(std::move(other.lease_)),
      captured_view_(std::move(other.captured_view_))
#if defined(MYRD_ENABLE_TEST_SEAMS)
      , phase_hook_(std::move(other.phase_hook_))
#endif
      , published_base_(std::move(other.published_base_)),
      journal_file_(std::move(other.journal_file_)),
      journal_header_(std::move(other.journal_header_)),
      journal_windows_identity_(std::move(other.journal_windows_identity_)),
      active_slot_(std::move(other.active_slot_)),
      journal_relative_name_(std::move(other.journal_relative_name_)),
      operation_id_(other.operation_id_) {
    other.lease_.reset();
    other.captured_view_ = {};
}

PreparedCompactionOperation& PreparedCompactionOperation::operator=(
    PreparedCompactionOperation&& other) noexcept {
    if (this != &other) {
        abandon_noexcept();
        lease_ = std::move(other.lease_);
        captured_view_ = std::move(other.captured_view_);
#if defined(MYRD_ENABLE_TEST_SEAMS)
        phase_hook_ = std::move(other.phase_hook_);
#endif
        published_base_ = std::move(other.published_base_);
        journal_file_ = std::move(other.journal_file_);
        journal_header_ = std::move(other.journal_header_);
        journal_windows_identity_ =
            std::move(other.journal_windows_identity_);
        active_slot_ = std::move(other.active_slot_);
        journal_relative_name_ = std::move(other.journal_relative_name_);
        operation_id_ = other.operation_id_;
        other.lease_.reset();
        other.captured_view_ = {};
    }
    return *this;
}

PreparedCompactionOperation::~PreparedCompactionOperation() {
    abandon_noexcept();
}

bool PreparedCompactionOperation::active() const noexcept {
    return lease_.has_value() && lease_->active() &&
        captured_view_.store != nullptr;
}

Result<void> PreparedCompactionOperation::abort() {
    journal_file_.reset();
    published_base_.reset();
    journal_relative_name_.clear();
    operation_id_ = 0;
    captured_view_ = {};
    if (!lease_.has_value()) return Result<void>::success();
    auto result = lease_->abort();
    lease_.reset();
    if (!result.ok()) return Result<void>::failure(result.error());
    return Result<void>::success();
}

void PreparedCompactionOperation::abandon_noexcept() noexcept {
    journal_file_.reset();
    published_base_.reset();
    journal_relative_name_.clear();
    operation_id_ = 0;
    captured_view_ = {};
    if (lease_.has_value()) {
        (void)lease_->abort();
        lease_.reset();
    }
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<void> PreparedCompactionOperation::emit_phase(
    const CompactionPhase phase) {
    return run_phase_hook(phase_hook_ ? &phase_hook_ : nullptr, phase);
}
#endif

Result<CompactionArtifactBuildMetrics>
PreparedCompactionOperation::build_artifacts(
    const OperationContext& context,
    const CompactionArtifactBuildRequest& request) {
    const auto fail = [this](Error error)
        -> Result<CompactionArtifactBuildMetrics> {
        (void)abort();
        return Result<CompactionArtifactBuildMetrics>::failure(
            std::move(error));
    };
    try {
    if (!active() || artifacts_ready()) {
        return fail(compaction_failure<void>(
            PersistenceErrorCode::OperationBusy,
            L"compaction operation cannot build artifacts in this state")
            .error());
    }
    std::stop_source combined_stop;
    const auto request_stop = [&combined_stop]() noexcept {
        combined_stop.request_stop();
    };
    std::stop_callback caller_stop(context.stop_token, request_stop);
    std::stop_callback session_stop(lease_->stop_token(), request_stop);
    const OperationContext operation_context{
        combined_stop.get_token(), context.deadline};
    auto live = check_operation_context(operation_context);
    if (!live.ok()) return fail(live.error());
    const auto composite = std::dynamic_pointer_cast<
        const CompositeBackingStore>(captured_view_.store);
    if (composite == nullptr) {
        return fail(compaction_failure<void>(
            PersistenceErrorCode::InvalidFormat,
            L"captured compaction source is not a composite durable view")
            .error());
    }
    const auto source_identity = composite->identity();
    auto valid = validate_artifact_request(
        request, source_identity, captured_view_.generation);
    if (!valid.ok()) return fail(valid.error());

    auto payload_source = CompactionPayloadSource::create(
        operation_context, captured_view_, source_identity,
        request.extent_options,
        request.limits.max_live_extents,
        request.vhdx_limits.max_candidate_payloads);
    if (!payload_source.ok()) return fail(payload_source.error());
    auto plan = plan_sparse_vhdx(
        operation_context,
        {source_identity.geometry.virtual_bytes,
         source_identity.geometry.logical_sector_size},
        request.base_identity,
        payload_source.value()->candidate_payloads(),
        *payload_source.value(), request.vhdx_limits);
    if (!plan.ok()) return fail(plan.error());

    const auto emit = [this](const CompactionPhase phase) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
        return emit_phase(phase);
#else
        (void)phase;
        return Result<void>::success();
#endif
    };
    auto offline_validator = [this, &request, &emit](
        const OperationContext& operation_context,
        const std::filesystem::path& path)
        -> Result<WindowsFileIdentity> {
        auto phase = emit(CompactionPhase::VhdxValidateAfter);
        if (!phase.ok()) {
            return Result<WindowsFileIdentity>::failure(phase.error());
        }
        phase = emit(CompactionPhase::VhdxWindowsValidateBefore);
        if (!phase.ok()) {
            return Result<WindowsFileIdentity>::failure(phase.error());
        }
        auto validated = request.offline_vhdx_validator(
            operation_context, path);
        if (!validated.ok()) return validated;
        phase = emit(CompactionPhase::VhdxWindowsValidateAfter);
        if (!phase.ok()) {
            return Result<WindowsFileIdentity>::failure(phase.error());
        }
        return validated;
    };

    Result<VhdxPublishedBase> published =
        Result<VhdxPublishedBase>::failure(
            compaction_failure<void>(
                PersistenceErrorCode::FatalInternalInvariant,
                L"VHDX publication did not execute").error());
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (phase_hook_) {
        VhdxPublicationTestOptions options{};
        options.phase_hook = [this](const VhdxPublicationPhase phase) {
            switch (phase) {
            case VhdxPublicationPhase::BeforeCreate:
                return emit_phase(CompactionPhase::VhdxCreateBefore);
            case VhdxPublicationPhase::BeforeClose:
                return emit_phase(CompactionPhase::VhdxReadbackAfter);
            case VhdxPublicationPhase::BeforeReopen:
                return emit_phase(CompactionPhase::VhdxValidateBefore);
            case VhdxPublicationPhase::BeforeRename:
                return emit_phase(CompactionPhase::VhdxRenameBefore);
            }
            return Result<void>::success();
        };
        options.file_write_hook = [this](const VhdxFileWritePhase phase) {
            switch (phase) {
            case VhdxFileWritePhase::BeforeExtend:
                return emit_phase(CompactionPhase::VhdxCreateAfter);
            case VhdxFileWritePhase::BeforePayload:
                return emit_phase(CompactionPhase::VhdxWriteBefore);
            case VhdxFileWritePhase::BeforeFlush: {
                auto emitted = emit_phase(CompactionPhase::VhdxWriteAfter);
                return emitted.ok()
                    ? emit_phase(CompactionPhase::VhdxFlushBefore)
                    : emitted;
            }
            case VhdxFileWritePhase::BeforeReadback: {
                auto emitted = emit_phase(CompactionPhase::VhdxFlushAfter);
                return emitted.ok()
                    ? emit_phase(CompactionPhase::VhdxReadbackBefore)
                    : emitted;
            }
            case VhdxFileWritePhase::BeforeStructure:
            case VhdxFileWritePhase::BeforeMetadata:
            case VhdxFileWritePhase::BeforeBat:
                return Result<void>::success();
            }
            return Result<void>::success();
        };
        options.pre_rename_validator = std::move(offline_validator);
        published = publish_sparse_vhdx_base_for_testing(
            operation_context, *request.writer_lock,
            request.base_temporary_name,
            request.base_relative_name, plan.value(),
            *payload_source.value(), std::move(options));
    } else
#endif
    {
        published = publish_sparse_vhdx_base(
            operation_context, *request.writer_lock,
            request.base_temporary_name,
            request.base_relative_name, plan.value(),
            *payload_source.value(), std::move(offline_validator));
    }
    if (!published.ok()) return fail(published.error());
    auto phase = emit(CompactionPhase::VhdxRenameAfter);
    if (!phase.ok()) return fail(phase.error());

    phase = emit(CompactionPhase::JournalCreateBefore);
    if (!phase.ok()) return fail(phase.error());
    auto journal = create_journal_epoch(
        operation_context, *request.writer_lock, request, source_identity,
        published.value(), emit);
    if (!journal.ok()) return fail(journal.error());
    phase = emit(CompactionPhase::FilesReady);
    if (!phase.ok()) return fail(phase.error());
    live = check_operation_context(operation_context);
    if (!live.ok()) return fail(live.error());

    CompactionArtifactBuildMetrics metrics{};
    metrics.source_generation = captured_view_.generation;
    metrics.extents = payload_source.value()->metrics();
    metrics.candidate_payloads =
        payload_source.value()->candidate_payloads().size();
    metrics.published_base_bytes = published.value().layout().file_length();
    metrics.journal_header_bytes = kJournalAlignment;
    published_base_ = published.move_value();
    journal_file_ = std::move(journal.value().file);
    journal_header_ = std::move(journal.value().header);
    journal_windows_identity_ =
        std::move(journal.value().windows_identity);
    active_slot_ = request.active_slot;
    journal_relative_name_ = request.journal_relative_name;
    operation_id_ = request.operation_id;
    return Result<CompactionArtifactBuildMetrics>::success(metrics);
    } catch (const std::bad_alloc&) {
        return fail(compaction_failure<void>(
            PersistenceErrorCode::SnapshotResourceExhausted,
            L"allocating bounded compaction artifact state failed")
            .error());
    }
}

Result<PreparedCompactionOperation> CompactionCoordinator::begin(
    const OperationContext& context) {
    return begin_impl(context
#if defined(MYRD_ENABLE_TEST_SEAMS)
        , nullptr
#endif
    );
}

CheckpointPrepareOutcome CompactionCoordinator::prepare_delta(
    const OperationContext& context,
    PreparedCompactionOperation&& operation,
    const CompactionDeltaPrepareRequest& request,
    JournalWriter& journal_writer) {
    const auto fail = [&](const Error& source) {
        (void)operation.abort();
        const auto code = compaction_error_code(source);
        auto error_context = request.correlation_context.value();
        error_context.operation = "compaction.prepare";
        error_context.code = code;
        error_context.phase = SessionPhase::Compacting;
        error_context.native_code = source.native_code;
        error_context.disposition = CommitDisposition::NotCommitted;
        error_context.safe_context =
            "pre-manifest compaction delta preparation failed";
        CheckpointOutcome outcome{};
        outcome.disposition = CommitDisposition::NotCommitted;
        outcome.generation = error_context.generation;
        outcome.error = Error{
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(code),
            L"compaction.prepare",
            source.message,
            source.native_code};
        outcome.error_context = std::move(error_context);
        return CheckpointPrepareOutcome{
            CheckpointPrepareFailure{std::move(outcome)}};
    };

    if (!operation.active() || !operation.artifacts_ready() ||
        !operation.lease_.has_value() ||
        operation.lease_->operation() != SessionOperation::Compaction ||
        operation.journal_file_ == nullptr ||
        operation.published_base_ == std::nullopt ||
        operation.operation_id_ == 0) {
        return fail(compaction_failure<void>(
            PersistenceErrorCode::OperationBusy,
            L"compaction artifacts are not ready for a consistent cut")
            .error());
    }
    const auto& correlation = request.correlation_context.value();
    const auto source_generation = operation.captured_view_.generation;
    if (source_generation >= kInvalidGeneration - 1 ||
        correlation.operation != "compaction.prepare" ||
        correlation.operation_id != operation.operation_id_ ||
        correlation.image_uuid != operation.journal_header_.image_uuid ||
        correlation.generation != source_generation + 1 ||
        correlation.disposition != CommitDisposition::NotCommitted ||
        request.snapshot_batch_bytes < kM2BlockSize ||
        request.snapshot_batch_bytes >
            store_->options().max_snapshot_batch_bytes) {
        return fail(compaction_failure<void>(
            PersistenceErrorCode::InvalidFormat,
            L"compaction delta request does not match prepared artifacts")
            .error());
    }

    const auto emit = [&operation](const CompactionPhase phase) {
#if defined(MYRD_ENABLE_TEST_SEAMS)
        return operation.emit_phase(phase);
#else
        (void)phase;
        return Result<void>::success();
#endif
    };
    auto phase = emit(CompactionPhase::BarrierValidateBefore);
    if (!phase.ok()) return fail(phase.error());
    if (!is_valid_barrier_for_checkpoint(request.volume_barrier)) {
        return fail(compaction_failure<void>(
            PersistenceErrorCode::InvalidFormat,
            L"compaction volume barrier evidence is missing or unknown")
            .error());
    }
    phase = emit(CompactionPhase::BarrierValidateAfter);
    if (!phase.ok()) return fail(phase.error());
    phase = emit(CompactionPhase::SnapshotBeginBefore);
    if (!phase.ok()) return fail(phase.error());

    JournalCheckpointRequest journal{};
    journal.file = operation.journal_file_;
    journal.file_identity = operation.journal_windows_identity_;
    journal.header = operation.journal_header_;
    journal.previous_generation = source_generation;
    journal.previous_digest = operation.journal_header_.predecessor_digest;
    journal.committed_length = kJournalAlignment;
    journal.root = {};
    journal.operation_id = operation.operation_id_;

    CheckpointPrepareRequest checkpoint_request{
        request.correlation_context};
    checkpoint_request.volume_barrier = request.volume_barrier;
    checkpoint_request.journal = std::move(journal);
    checkpoint_request.snapshot_batch_bytes = request.snapshot_batch_bytes;

    const auto& base = *operation.published_base_;
    if (!base.capability_anchor() ||
        !same_file_object(
            base.capability_anchor()->identity(), base.windows_identity()) ||
        base.capability_anchor()->identity().size !=
            base.windows_identity().size) {
        return fail(compaction_failure<void>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"published base lost its handle-anchored identity")
            .error());
    }
    ManifestArtifactReplacement replacement{};
    replacement.source_active_slot = operation.active_slot_;
    replacement.base_generation = source_generation;
    replacement.base_relative_name = base.relative_name();
    replacement.base_identity = base.layout().file_identity();
    replacement.base_windows_identity = base.windows_identity();
    replacement.base_expected_length = base.layout().file_length();
    replacement.base_capability_anchor = base.capability_anchor();
    replacement.journal_relative_name = operation.journal_relative_name_;
    replacement.journal_identity = operation.journal_header_.journal_identity;
    replacement.journal_epoch = operation.journal_header_.epoch;

    CheckpointTransactionObservers observers{};
    std::function<Result<void>(const ConsistentSnapshotToken&)>
        after_snapshot_begin;
    std::function<Result<void>()> after_journal_prepare;
    try {
#if defined(MYRD_ENABLE_TEST_SEAMS)
        auto retained_hook =
            std::make_shared<CompactionPhaseHook>(operation.phase_hook_);
        const auto observe = [retained_hook](const CompactionPhase phase) {
            return run_phase_hook(retained_hook.get(), phase);
        };
        observers.before_manifest_publish = [observe]() {
            return observe(CompactionPhase::ManifestPublishBefore);
        };
        observers.after_manifest_publish = [observe]() {
            return observe(CompactionPhase::ManifestPublishAfter);
        };
        observers.before_backing_install = [observe]() {
            return observe(CompactionPhase::BackingInstallBefore);
        };
        observers.after_backing_install = [observe]() {
            return observe(CompactionPhase::BackingInstallAfter);
        };
        observers.before_conditional_ack = [observe]() {
            return observe(CompactionPhase::ConditionalAckBefore);
        };
        observers.after_conditional_ack = [observe]() {
            return observe(CompactionPhase::ConditionalAckAfter);
        };
        observers.before_snapshot_finish = [observe]() {
            return observe(CompactionPhase::SnapshotFinishBefore);
        };
        observers.after_snapshot_finish = [observe]() {
            return observe(CompactionPhase::SnapshotFinishAfter);
        };
        observers.complete = [observe]() {
            return observe(CompactionPhase::Complete);
        };
#endif
        after_snapshot_begin = [&emit, &request](
            const ConsistentSnapshotToken& token) {
            auto emitted = emit(CompactionPhase::SnapshotBeginAfter);
            if (!emitted.ok()) return emitted;
            if (request.after_snapshot_begin) {
                auto published = request.after_snapshot_begin(token);
                if (!published.ok()) return published;
            }
            return emit(CompactionPhase::DeltaWriteBefore);
        };
        after_journal_prepare = [&emit]() {
            return emit(CompactionPhase::DeltaWriteAfter);
        };
    } catch (const std::bad_alloc&) {
        return fail(compaction_failure<void>(
            PersistenceErrorCode::IoError,
            L"allocating compaction transaction observers failed")
            .error());
    }

    auto lease = std::move(*operation.lease_);
    operation.lease_.reset();
    CheckpointCoordinator checkpoint(
        *session_, *store_, journal_writer);
    auto prepared = checkpoint.prepare_with_lease(
        context, checkpoint_request, std::move(lease),
        std::move(after_snapshot_begin),
        std::move(after_journal_prepare),
        std::move(observers),
        std::move(replacement));

    operation.journal_file_.reset();
    operation.published_base_.reset();
    operation.captured_view_ = {};
    operation.journal_relative_name_.clear();
    operation.operation_id_ = 0;
    return prepared;
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<PreparedCompactionOperation>
CompactionCoordinator::begin_for_testing(
    const OperationContext& context,
    CompactionPhaseHook hook) {
    if (!hook) {
        return compaction_failure<PreparedCompactionOperation>(
            PersistenceErrorCode::InvalidFormat,
            L"compaction test phase hook is empty");
    }
    return begin_impl(context, &hook);
}
#endif

Result<PreparedCompactionOperation> CompactionCoordinator::begin_impl(
    const OperationContext& context
#if defined(MYRD_ENABLE_TEST_SEAMS)
    , const CompactionPhaseHook* hook
#endif
) {
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<PreparedCompactionOperation>::failure(live.error());
    }
    auto begun = session_->begin_compaction();
    if (!begun.ok()) {
        return Result<PreparedCompactionOperation>::failure(begun.error());
    }
    auto lease = begun.move_value();

#if defined(MYRD_ENABLE_TEST_SEAMS)
    auto phase = run_phase_hook(
        hook, CompactionPhase::OperationGateAcquired);
    if (!phase.ok()) {
        return Result<PreparedCompactionOperation>::failure(phase.error());
    }
#endif

    auto captured = store_->capture_backing_view();
    if (captured.store == nullptr ||
        captured.generation == kInvalidGeneration) {
        return compaction_failure<PreparedCompactionOperation>(
            PersistenceErrorCode::InvalidFormat,
            L"compaction requires a valid immutable durable backing view");
    }

#if defined(MYRD_ENABLE_TEST_SEAMS)
    phase = run_phase_hook(
        hook, CompactionPhase::DurableViewCaptured);
    if (!phase.ok()) {
        return Result<PreparedCompactionOperation>::failure(phase.error());
    }
#endif

    live = check_operation_context(context);
    if (!live.ok()) {
        return Result<PreparedCompactionOperation>::failure(live.error());
    }
    return Result<PreparedCompactionOperation>::success(
        [&]() {
            PreparedCompactionOperation operation{
                std::move(lease), std::move(captured)};
#if defined(MYRD_ENABLE_TEST_SEAMS)
            if (hook != nullptr) operation.phase_hook_ = *hook;
#endif
            return operation;
        }());
}

CompactionRecommendation should_compact(
    const CompactionMetrics& metrics,
    const CompactionPolicy& policy) noexcept {
    CompactionRecommendation result{};

    result.journal_physical_bytes =
        policy.journal_physical_bytes_at_or_above.has_value() &&
        metrics.journal_physical_bytes >=
            *policy.journal_physical_bytes_at_or_above;
    result.checkpoint_count = policy.checkpoints_at_or_above.has_value() &&
        metrics.checkpoints_since_compaction >= *policy.checkpoints_at_or_above;
    result.index_depth = policy.index_depth_at_or_above.has_value() &&
        metrics.index_depth >= *policy.index_depth_at_or_above;

    if (policy.journal_to_base_ratio_at_or_above.has_value()) {
        const auto threshold = *policy.journal_to_base_ratio_at_or_above;
        if (threshold.numerator == 0 || threshold.denominator == 0) {
            result.policy_valid = false;
        } else if (metrics.base_physical_bytes == 0) {
            // A non-empty journal over an empty base is an infinite ratio;
            // define 0/0 as no pressure rather than an automatic trigger.
            result.journal_to_base_ratio =
                metrics.journal_physical_bytes != 0;
        } else {
            result.journal_to_base_ratio = compare_positive_fractions(
                metrics.journal_physical_bytes,
                metrics.base_physical_bytes,
                threshold.numerator,
                threshold.denominator) >= 0;
        }
    }

    result.recommended = result.policy_valid &&
        (result.journal_physical_bytes || result.journal_to_base_ratio ||
         result.checkpoint_count || result.index_depth);
    return result;
}

}  // namespace myrd::persistence
