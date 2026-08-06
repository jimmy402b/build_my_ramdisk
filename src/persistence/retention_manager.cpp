#include "retention_manager.h"

#include "src/persistence/image_paths.h"
#include "src/persistence/image_session.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/vhdx_format.h"
#include "src/persistence/writer_lock.h"

#include <algorithm>
#include <new>

namespace myrd::persistence {
namespace {

template <class T>
Result<T> retention_failure(
    const PersistenceErrorCode code,
    const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"retention.reference",
        message,
        0});
}

template <std::size_t N>
bool all_zero(const std::array<std::byte, N>& value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{};
    });
}

bool all_zero(const std::span<const std::byte> value) noexcept {
    return std::all_of(value.begin(), value.end(), [](const std::byte byte) {
        return byte == std::byte{};
    });
}

std::optional<bool> aliases(
    const std::string_view left,
    const std::string_view right) noexcept {
    try {
        auto left_component = validate_image_component(left);
        auto right_component = validate_image_component(right);
        if (!left_component.ok() || !right_component.ok()) {
            return std::nullopt;
        }
        return std::optional<bool>{CompareStringOrdinal(
            left_component.value().wide_name.data(),
            static_cast<int>(left_component.value().wide_name.size()),
            right_component.value().wide_name.data(),
            static_cast<int>(right_component.value().wide_name.size()),
            TRUE) == CSTR_EQUAL};
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<bool> same_artifact(
    const OwnedImmutableArtifact& left,
    const OwnedImmutableArtifact& right) noexcept {
    if (left.kind != right.kind ||
        !same_file_object(left.windows_identity, right.windows_identity)) {
        return false;
    }
    return aliases(left.relative_name, right.relative_name);
}

std::optional<bool> manifest_references(
    const ManifestSlot& slot,
    const OwnedImmutableArtifact& candidate) noexcept {
    if (candidate.kind == ImmutableArtifactKind::BaseVhdx) {
        return aliases(slot.base_relative_name, candidate.relative_name);
    }
    return aliases(slot.journal_relative_name, candidate.relative_name);
}

bool live_view_references(
    const CompositeBackingIdentity& view,
    const OwnedImmutableArtifact& candidate) noexcept {
    const auto& identity = candidate.kind == ImmutableArtifactKind::BaseVhdx
        ? view.base_windows_identity
        : view.journal_windows_identity;
    return same_file_object(identity, candidate.windows_identity);
}

bool ownership_matches(
    const OwnedImmutableArtifact& candidate,
    const RetentionReferenceSnapshot& references) noexcept {
    if (candidate.image_uuid.is_nil() ||
        candidate.image_uuid != references.image_uuid ||
        candidate.windows_identity.volume_serial_number == 0 ||
        all_zero(candidate.windows_identity.file_id)) {
        return false;
    }
    if (candidate.kind == ImmutableArtifactKind::BaseVhdx) {
        return !all_zero(candidate.base_identity.file_write_guid) &&
            !all_zero(candidate.base_identity.data_write_guid) &&
            !all_zero(candidate.virtual_disk_id) &&
            candidate.virtual_disk_id == references.virtual_disk_id;
    }
    return !all_zero(candidate.journal_identity.journal_uuid) &&
        candidate.journal_epoch != 0 &&
        candidate.journal_epoch != kInvalidGeneration;
}

RetentionCleanupEntry kept_error(
    const OwnedImmutableArtifact& candidate,
    const Error& error) {
    RetentionCleanupEntry entry{};
    entry.kind = candidate.kind;
    entry.relative_name = candidate.relative_name;
    entry.decision = RetentionDecision::Eligible;
    entry.action = RetentionCleanupAction::Kept;
    entry.has_error = true;
    entry.error_domain = error.domain;
    entry.error_code = error.code;
    entry.native_code = error.native_code;
    return entry;
}

Result<std::shared_ptr<IPersistenceFile>> validate_owned_artifact(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const OwnedImmutableArtifact& candidate) {
    auto capability = open_bundle_file_for_inspection(
        bundle_root, candidate.relative_name, candidate.windows_identity);
    if (!capability.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            capability.error());
    }
    auto opened = open_validated_persistence_file(capability.move_value());
    if (!opened.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            opened.error());
    }
    auto file = std::shared_ptr<IPersistenceFile>(opened.move_value());
    if (candidate.kind == ImmutableArtifactKind::BaseVhdx) {
        VhdxParseEvidence evidence{};
        const VhdxParseRequirements requirements{
            candidate.base_identity, candidate.windows_identity.size};
        auto layout = parse_vhdx_layout(
            context, file, requirements, &evidence);
        if (!layout.ok()) {
            return Result<std::shared_ptr<IPersistenceFile>>::failure(
                layout.error());
        }
        if (layout.value().virtual_disk_id() != candidate.virtual_disk_id) {
            return retention_failure<std::shared_ptr<IPersistenceFile>>(
                PersistenceErrorCode::GenerationConflict,
                L"owned base virtual-disk identity changed");
        }
        return Result<std::shared_ptr<IPersistenceFile>>::success(
            std::move(file));
    }

    JournalHeaderBytes bytes{};
    auto read = file->read_exact(context, 0, bytes);
    if (!read.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            read.error());
    }
    auto header = decode_journal_header(bytes);
    if (!header.ok()) {
        return Result<std::shared_ptr<IPersistenceFile>>::failure(
            header.error());
    }
    if (header.value().image_uuid != candidate.image_uuid ||
        header.value().journal_identity != candidate.journal_identity ||
        header.value().epoch != candidate.journal_epoch) {
        return retention_failure<std::shared_ptr<IPersistenceFile>>(
            PersistenceErrorCode::GenerationConflict,
            L"owned journal identity changed");
    }
    return Result<std::shared_ptr<IPersistenceFile>>::success(
        std::move(file));
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
Result<void> run_retention_hook(
    const RetentionCleanupOptions& options,
    const RetentionCleanupTestPhase phase) {
    if (!options.phase_hook) return Result<void>::success();
    try {
        return options.phase_hook(phase);
    } catch (...) {
        return retention_failure<void>(
            PersistenceErrorCode::FatalInternalInvariant,
            L"retention cleanup phase hook threw");
    }
}
#endif

} // namespace

const char* to_string(const RetentionDecision decision) noexcept {
    switch (decision) {
    case RetentionDecision::Eligible: return "Eligible";
    case RetentionDecision::InvalidName: return "InvalidName";
    case RetentionDecision::ReparsePoint: return "ReparsePoint";
    case RetentionDecision::HardLinked: return "HardLinked";
    case RetentionDecision::UnknownOwnership: return "UnknownOwnership";
    case RetentionDecision::ManifestReferenced: return "ManifestReferenced";
    case RetentionDecision::LiveViewReferenced: return "LiveViewReferenced";
    case RetentionDecision::InFlightReferenced: return "InFlightReferenced";
    }
    return "UnknownRetentionDecision";
}

Result<RetentionLiveViewReference> make_retention_live_view_reference(
    const BackingView& view) {
    if (!view.store) {
        return retention_failure<RetentionLiveViewReference>(
            PersistenceErrorCode::InvalidFormat,
            L"live retention view has no backing store");
    }
    const auto composite = std::dynamic_pointer_cast<
        const CompositeBackingStore>(view.store);
    if (!composite || composite->identity().checkpoint_generation !=
            view.generation) {
        return retention_failure<RetentionLiveViewReference>(
            PersistenceErrorCode::GenerationConflict,
            L"live retention view identity does not match its generation");
    }
    return Result<RetentionLiveViewReference>::success({
        composite->identity(), std::weak_ptr<const IBackingStore>{view.store}});
}

RetentionDecision evaluate_retention_candidate(
    const OwnedImmutableArtifact& candidate,
    const RetentionReferenceSnapshot& references) noexcept {
    const auto component = validate_image_component(candidate.relative_name);
    if (!component.ok()) return RetentionDecision::InvalidName;
    if ((candidate.windows_identity.attributes &
         FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        candidate.windows_identity.reparse_tag != 0) {
        return RetentionDecision::ReparsePoint;
    }
    if (candidate.windows_identity.hard_link_count != 1) {
        return RetentionDecision::HardLinked;
    }
    if (!ownership_matches(candidate, references)) {
        return RetentionDecision::UnknownOwnership;
    }
    for (const auto& slot : references.valid_slots) {
        if (slot.has_value()) {
            const auto referenced = manifest_references(*slot, candidate);
            if (!referenced.has_value()) {
                return RetentionDecision::UnknownOwnership;
            }
            if (*referenced) {
                return RetentionDecision::ManifestReferenced;
            }
        }
    }
    for (const auto& live : references.live_views) {
        const auto retained = live.lifetime.lock();
        if (retained && live_view_references(live.identity, candidate)) {
            return RetentionDecision::LiveViewReferenced;
        }
    }
    for (const auto& in_flight : references.in_flight_artifacts) {
        const auto referenced = same_artifact(in_flight, candidate);
        if (!referenced.has_value()) {
            return RetentionDecision::UnknownOwnership;
        }
        if (*referenced) {
            return RetentionDecision::InFlightReferenced;
        }
    }
    return RetentionDecision::Eligible;
}

namespace {

Result<RetentionReferenceSnapshot> capture_reference_snapshot(
    const OperationContext& context,
    BundleWriterLock& writer,
    DynamicBlockStore& store,
    const std::filesystem::path& bundle_root,
    const std::string_view manifest_relative_name,
    const std::size_t max_live_views,
    const RecoveryOptions& recovery_options) {
    auto manifest = open_bundle_file_for_inspection(
        bundle_root, manifest_relative_name, writer.manifest_identity());
    if (!manifest.ok()) {
        return Result<RetentionReferenceSnapshot>::failure(manifest.error());
    }
    auto opened = open_validated_persistence_file(manifest.move_value());
    if (!opened.ok()) {
        return Result<RetentionReferenceSnapshot>::failure(opened.error());
    }
    auto file = std::shared_ptr<IPersistenceFile>(opened.move_value());
    std::array<std::byte, kManifestFileBytes> bytes{};
    auto read = file->read_exact(context, 0, bytes);
    if (!read.ok()) {
        return Result<RetentionReferenceSnapshot>::failure(read.error());
    }

    RetentionReferenceSnapshot snapshot{};
    std::size_t decoded_slots{};
    for (std::size_t index = 0; index < snapshot.valid_slots.size(); ++index) {
        const auto offset = index * kManifestSlotBytes;
        const auto slot_bytes = std::span<const std::byte>{bytes}.subspan(
            offset, kManifestSlotBytes);
        if (all_zero(slot_bytes)) continue;
        auto inspected = inspect_manifest_slot(slot_bytes);
        if (!inspected.ok() ||
            inspected.value().status !=
                ManifestSlotInspectionStatus::DecodedV1 ||
            !inspected.value().slot.has_value()) {
            return retention_failure<RetentionReferenceSnapshot>(
                PersistenceErrorCode::ManifestCorrupt,
                L"nonempty manifest slot is not safely inspectable");
        }
        auto slot = *inspected.value().slot;
        auto semantics = validate_manifest_slot_semantics(slot);
        auto paths = validate_manifest_slot_paths(slot);
        if (!semantics.ok() || !paths.ok()) {
            return retention_failure<RetentionReferenceSnapshot>(
                PersistenceErrorCode::ManifestCorrupt,
                L"nonempty manifest slot is not safe for retention");
        }
        snapshot.valid_slots[index] = std::move(slot);
        ++decoded_slots;
    }
    if (decoded_slots == 0) {
        return retention_failure<RetentionReferenceSnapshot>(
            PersistenceErrorCode::ManifestCorrupt,
            L"retention found no valid manifest slot");
    }

    auto recovered = recover_complete_image(
        context, bundle_root, manifest_relative_name, recovery_options);
    if (!recovered.ok() || !recovered.image.has_value()) {
        return retention_failure<RetentionReferenceSnapshot>(
            PersistenceErrorCode::VerificationFailed,
            L"normal recovery could not prove a current retention identity");
    }
    snapshot.image_uuid = recovered.image->image_uuid;
    snapshot.virtual_disk_id =
        recovered.image->backing_identity.virtual_disk_id;
    for (const auto& slot : snapshot.valid_slots) {
        if (slot.has_value() && slot->image_uuid != snapshot.image_uuid) {
            return retention_failure<RetentionReferenceSnapshot>(
                PersistenceErrorCode::GenerationConflict,
                L"manifest slots disagree on retention image identity");
        }
    }

    auto live_views = store.capture_live_backing_views(max_live_views);
    if (!live_views.ok()) {
        return Result<RetentionReferenceSnapshot>::failure(
            live_views.error());
    }
    if (live_views.value().empty()) {
        return retention_failure<RetentionReferenceSnapshot>(
            PersistenceErrorCode::GenerationConflict,
            L"retention runtime has no current backing view");
    }
    snapshot.live_views.reserve(live_views.value().size());
    for (std::size_t index = 0; index < live_views.value().size(); ++index) {
        const auto& view = live_views.value()[index];
        const auto composite = std::dynamic_pointer_cast<
            const CompositeBackingStore>(view.store);
        if (!composite) {
            return retention_failure<RetentionReferenceSnapshot>(
                PersistenceErrorCode::GenerationConflict,
                L"a live runtime backing has unknown file references");
        }
        if (index == 0 &&
            composite->identity() != recovered.image->backing_identity) {
            return retention_failure<RetentionReferenceSnapshot>(
                PersistenceErrorCode::GenerationConflict,
                L"runtime backing does not match recovered current view");
        }
        auto reference = make_retention_live_view_reference(view);
        if (!reference.ok() ||
            reference.value().identity.image_uuid != snapshot.image_uuid) {
            return retention_failure<RetentionReferenceSnapshot>(
                PersistenceErrorCode::GenerationConflict,
                L"runtime live views disagree on image identity");
        }
        snapshot.live_views.push_back(reference.move_value());
    }
    return Result<RetentionReferenceSnapshot>::success(std::move(snapshot));
}

Result<RetentionCleanupReport> cleanup_with_snapshot(
    const OperationContext& context,
    PublishOperationLease cleanup_lease,
    BundleWriterLock& writer,
    const std::filesystem::path& bundle_root,
    const RetentionReferenceSnapshot& references,
    const std::span<const OwnedImmutableArtifact> candidates,
    const RetentionCleanupOptions options) {
    try {
        if (!cleanup_lease.active() ||
            cleanup_lease.operation() != SessionOperation::Cleanup) {
            return retention_failure<RetentionCleanupReport>(
                PersistenceErrorCode::OperationBusy,
                L"retention cleanup requires its exclusive session lease");
        }
        if (bundle_root.empty() || options.max_candidates == 0 ||
            candidates.size() > options.max_candidates) {
            return retention_failure<RetentionCleanupReport>(
                PersistenceErrorCode::SnapshotResourceExhausted,
                L"retention candidate budget is invalid or exceeded");
        }
        auto writer_valid = writer.verify();
        if (!writer_valid.ok()) {
            return Result<RetentionCleanupReport>::failure(
                writer_valid.error());
        }
        RetentionCleanupReport report{};
        report.dry_run = options.dry_run;
        report.candidate_count = candidates.size();
        report.entries.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            auto live = check_operation_context(context);
            if (!live.ok()) {
                return Result<RetentionCleanupReport>::failure(live.error());
            }
            const auto decision = evaluate_retention_candidate(
                candidate, references);
            RetentionCleanupEntry entry{};
            entry.kind = candidate.kind;
            entry.relative_name = candidate.relative_name;
            entry.decision = decision;
            if (decision != RetentionDecision::Eligible) {
                report.entries.push_back(std::move(entry));
                continue;
            }
            ++report.eligible_count;
            auto before_capability = check_operation_context(context);
            if (!before_capability.ok()) {
                return Result<RetentionCleanupReport>::failure(
                    before_capability.error());
            }
            std::optional<ValidatedBundleFile> mutation;
            std::optional<ValidatedBundleFile> dry_run_anchor;
            if (options.dry_run) {
                auto anchor = open_bundle_file_for_immutable_anchor(
                    writer, candidate.relative_name,
                    candidate.windows_identity);
                if (!anchor.ok()) {
                    report.entries.push_back(kept_error(
                        candidate, anchor.error()));
                    continue;
                }
                dry_run_anchor = anchor.move_value();
            } else {
                auto opened_mutation = open_bundle_file_for_mutation(
                    writer, candidate.relative_name,
                    candidate.windows_identity);
                if (!opened_mutation.ok()) {
                    report.entries.push_back(kept_error(
                        candidate, opened_mutation.error()));
                    continue;
                }
                mutation = opened_mutation.move_value();
            }

#if defined(MYRD_ENABLE_TEST_SEAMS)
            auto protected_phase = run_retention_hook(
                options,
                RetentionCleanupTestPhase::AfterProtectionBeforeValidation);
            if (!protected_phase.ok()) {
                return Result<RetentionCleanupReport>::failure(
                    protected_phase.error());
            }
#endif

            // The immutable or mutation capability already denies content
            // writes. Parse a second read handle to the same exact file object
            // while that pin remains held, closing the content-TOCTOU window.
            auto inspection = validate_owned_artifact(
                context, bundle_root, candidate);
            if (!inspection.ok()) {
                report.entries.push_back(kept_error(
                    candidate, inspection.error()));
                continue;
            }
            if (options.dry_run) {
                report.entries.push_back(std::move(entry));
                continue;
            }
#if defined(MYRD_ENABLE_TEST_SEAMS)
            auto before_delete_phase = run_retention_hook(
                options, RetentionCleanupTestPhase::BeforeDelete);
            if (!before_delete_phase.ok()) {
                return Result<RetentionCleanupReport>::failure(
                    before_delete_phase.error());
            }
#endif
            auto before_delete = check_operation_context(context);
            if (!before_delete.ok()) {
                return Result<RetentionCleanupReport>::failure(
                    before_delete.error());
            }
            ++report.delete_attempt_count;
            entry.delete_attempted = true;
            auto deleted = delete_validated_bundle_file(
                writer, *mutation);
            if (!deleted.ok()) {
                entry = kept_error(candidate, deleted.error());
                entry.delete_attempted = true;
                report.entries.push_back(std::move(entry));
                continue;
            }
            entry.action = RetentionCleanupAction::Deleted;
            ++report.deleted_count;
            report.entries.push_back(std::move(entry));
        }
        auto released = cleanup_lease.abort();
        if (!released.ok()) {
            return Result<RetentionCleanupReport>::failure(released.error());
        }
        return Result<RetentionCleanupReport>::success(std::move(report));
    } catch (const std::bad_alloc&) {
        return retention_failure<RetentionCleanupReport>(
            PersistenceErrorCode::IoError,
            L"allocating retention cleanup report failed");
    }
}

template <class Capture>
Result<RetentionCleanupReport> run_cleanup_with_capture(
    const OperationContext& caller_context,
    PublishOperationLease cleanup_lease,
    BundleWriterLock& writer,
    const std::filesystem::path& bundle_root,
    const std::span<const OwnedImmutableArtifact> candidates,
    const RetentionCleanupOptions options,
    Capture&& capture) {
    try {
    std::stop_source combined_stop;
    const auto request_stop = [&combined_stop]() noexcept {
        combined_stop.request_stop();
    };
    std::stop_callback caller_stop(
        caller_context.stop_token, request_stop);
    std::stop_callback session_stop(
        cleanup_lease.stop_token(), request_stop);
    const OperationContext context{
        combined_stop.get_token(), caller_context.deadline};
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<RetentionCleanupReport>::failure(live.error());
    }
    auto references = capture(context);
    if (!references.ok()) {
        return Result<RetentionCleanupReport>::failure(references.error());
    }
    return cleanup_with_snapshot(
        context, std::move(cleanup_lease), writer, bundle_root,
        references.value(), candidates, options);
    } catch (const std::bad_alloc&) {
        return retention_failure<RetentionCleanupReport>(
            PersistenceErrorCode::SnapshotResourceExhausted,
            L"allocating retention reference state failed");
    }
}

} // namespace

Result<RetentionCleanupReport> cleanup_retention_artifacts(
    const OperationContext& context,
    PublishOperationLease cleanup_lease,
    BundleWriterLock& writer,
    DynamicBlockStore& store,
    const std::filesystem::path& bundle_root,
    const std::string_view manifest_relative_name,
    const std::span<const OwnedImmutableArtifact> candidates,
    const RetentionCleanupOptions options) {
    return run_cleanup_with_capture(
        context, std::move(cleanup_lease), writer, bundle_root,
        candidates, options,
        [&](const OperationContext& operation_context) {
            return capture_reference_snapshot(
                operation_context, writer, store, bundle_root,
                manifest_relative_name, options.max_live_views,
                options.recovery);
        });
}

#if defined(MYRD_ENABLE_TEST_SEAMS)
namespace test {

Result<RetentionCleanupReport> cleanup_retention_artifacts_with_snapshot(
    const OperationContext& context,
    PublishOperationLease cleanup_lease,
    BundleWriterLock& writer,
    const std::filesystem::path& bundle_root,
    const RetentionReferenceSnapshot& references,
    const std::span<const OwnedImmutableArtifact> candidates,
    const RetentionCleanupOptions options) {
    return run_cleanup_with_capture(
        context, std::move(cleanup_lease), writer, bundle_root,
        candidates, options,
        [&](const OperationContext&) {
            return Result<RetentionReferenceSnapshot>::success(references);
        });
}

} // namespace test
#endif

} // namespace myrd::persistence
