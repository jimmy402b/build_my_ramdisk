#include "src/persistence/commit_resolver.h"

#include "src/persistence/image_paths.h"
#include "src/persistence/manifest_selector.h"
#include "src/persistence/recovery_engine.h"
#include "src/persistence/writer_lock.h"

#include <array>
#include <utility>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace myrd::persistence {
namespace {

template <class T>
Result<T> candidate_failure(const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(PersistenceErrorCode::InvalidFormat),
        L"prepare_manifest_candidate",
        message,
        0});
}

Error publish_error(
    const PersistenceErrorCode code,
    const wchar_t* message,
    const std::uint32_t native_code = 0) {
    return {
        ErrorDomain::persistence,
        static_cast<std::uint32_t>(code),
        L"attempt_manifest_publication",
        message,
        native_code};
}

ManifestPublishAttemptOutcome publish_failure(
    const ManifestPublishBoundary boundary,
    Error error) {
    return {boundary, false, std::move(error)};
}

}  // namespace

Result<PreparedManifestCandidate> prepare_manifest_candidate(
    const ManifestSlot& active_slot,
    const PreparedCheckpoint& checkpoint,
    const std::uint64_t committed_time_100ns,
    const std::optional<ManifestArtifactReplacement>& replacement) {
    if (!validate_manifest_slot_semantics(active_slot).ok() ||
        active_slot.slot_index > 1 || committed_time_100ns == 0 ||
        checkpoint.generation == 0 ||
        checkpoint.generation == kInvalidGeneration ||
        active_slot.generation.checkpoint_generation >=
            kInvalidGeneration - 1 ||
        checkpoint.generation !=
            active_slot.generation.checkpoint_generation + 1 ||
        (!replacement.has_value() &&
         checkpoint.committed_length <
             active_slot.generation.journal.committed_length) ||
        checkpoint.commit.length != kJournalAlignment ||
        checkpoint.commit.offset > checkpoint.committed_length ||
        checkpoint.commit.length >
            checkpoint.committed_length - checkpoint.commit.offset ||
        checkpoint.commit.offset + checkpoint.commit.length !=
            checkpoint.committed_length ||
        checkpoint.commit.chain_digest != checkpoint.chain_digest) {
        return candidate_failure<PreparedManifestCandidate>(
            L"active slot and prepared checkpoint do not form one successor");
    }

    ManifestSlot candidate = active_slot;
    candidate.slot_index = static_cast<std::uint8_t>(
        active_slot.slot_index == 0 ? 1 : 0);
    candidate.generation.checkpoint_generation = checkpoint.generation;
    if (replacement.has_value()) {
        const auto& artifacts = *replacement;
        auto base_name = validate_image_component(
            artifacts.base_relative_name);
        auto journal_name = validate_image_component(
            artifacts.journal_relative_name);
        auto source_base_name = validate_image_component(
            artifacts.source_active_slot.base_relative_name);
        auto source_journal_name = validate_image_component(
            artifacts.source_active_slot.journal_relative_name);
        const auto aliases = [](const Result<ImageComponent>& left,
                                const Result<ImageComponent>& right) {
            return left.ok() && right.ok() &&
            CompareStringOrdinal(
                left.value().wide_name.data(),
                static_cast<int>(left.value().wide_name.size()),
                right.value().wide_name.data(),
                static_cast<int>(right.value().wide_name.size()),
                TRUE) == CSTR_EQUAL;
        };
        const auto names_alias =
            aliases(base_name, journal_name) ||
            aliases(base_name, source_base_name) ||
            aliases(base_name, source_journal_name) ||
            aliases(journal_name, source_base_name) ||
            aliases(journal_name, source_journal_name);
        if (!base_name.ok() || !journal_name.ok() ||
            !source_base_name.ok() || !source_journal_name.ok() ||
            artifacts.source_active_slot != active_slot ||
            artifacts.base_generation !=
                active_slot.generation.checkpoint_generation ||
            artifacts.base_expected_length == 0 ||
            artifacts.journal_epoch == 0 ||
            active_slot.generation.journal.epoch >= kInvalidGeneration - 1 ||
            artifacts.journal_epoch !=
                active_slot.generation.journal.epoch + 1 ||
            artifacts.base_identity == active_slot.base_identity ||
            artifacts.journal_identity == active_slot.journal_identity ||
            names_alias) {
            return candidate_failure<PreparedManifestCandidate>(
                L"compaction replacement artifacts are invalid");
        }
        candidate.generation.base_generation = artifacts.base_generation;
        candidate.base_relative_name = artifacts.base_relative_name;
        candidate.base_identity = artifacts.base_identity;
        candidate.base_expected_length = artifacts.base_expected_length;
        candidate.journal_relative_name = artifacts.journal_relative_name;
        candidate.journal_identity = artifacts.journal_identity;
        candidate.generation.journal.epoch = artifacts.journal_epoch;
    }
    candidate.generation.journal.committed_length =
        checkpoint.committed_length;
    candidate.generation.journal.commit = checkpoint.commit;
    candidate.generation.journal.root = checkpoint.root;
    candidate.committed_time_100ns = committed_time_100ns;
    auto encoded = encode_manifest_slot(candidate);
    if (!encoded.ok()) {
        return Result<PreparedManifestCandidate>::failure(encoded.error());
    }
    return Result<PreparedManifestCandidate>::success({
        std::move(candidate),
        encoded.move_value(),
        static_cast<std::uint64_t>(
            active_slot.slot_index == 0 ? kManifestSlotBytes : 0),
    });
}

ManifestPublishAttemptOutcome attempt_manifest_publication(
    const OperationContext& context,
    PublishOperationLease& lease,
    const ManifestPublishRequest& request) {
    constexpr auto before =
        ManifestPublishBoundary::BeforeSlotWriteAttempt;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    const auto injected = [&](const ManifestPublishTestPhase phase,
                              const ManifestPublishBoundary boundary)
        -> std::optional<ManifestPublishAttemptOutcome> {
        if (!request.test_phase_hook) return std::nullopt;
        Result<void> result = Result<void>::success();
        try {
            result = request.test_phase_hook(phase);
        } catch (...) {
            result = Result<void>::failure(publish_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"manifest phase hook threw an exception"));
        }
        if (result.ok()) return std::nullopt;
        return publish_failure(boundary, result.error());
    };
#endif
    const auto canonical_candidate =
        encode_manifest_slot(request.candidate.slot);
    bool validation_configured =
        request.complete_validation.has_value() &&
        !request.complete_validation->bundle_root.empty() &&
        validate_image_component(
            request.complete_validation->manifest_relative_name).ok() &&
        validate_limits(request.complete_validation->options.limits) ==
            LimitsStatus::Ok;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    validation_configured = validation_configured ||
        static_cast<bool>(request.test_validate_candidate);
#endif
    if (!request.manifest_file || !validation_configured ||
        !canonical_candidate.ok() ||
        canonical_candidate.value() != request.candidate.bytes ||
        !validate_manifest_slot_semantics(request.active_slot).ok() ||
        !validate_manifest_slot_semantics(request.candidate.slot).ok() ||
        request.active_slot.slot_index > 1 ||
        request.candidate.slot.slot_index > 1 ||
        request.candidate.slot.slot_index == request.active_slot.slot_index ||
        request.candidate.file_offset !=
            static_cast<std::uint64_t>(request.candidate.slot.slot_index) *
                kManifestSlotBytes) {
        return publish_failure(before, publish_error(
            PersistenceErrorCode::InvalidFormat,
            L"manifest publication request is invalid"));
    }
    auto live = check_operation_context(context);
    if (!live.ok()) return publish_failure(before, live.error());
    const auto length = request.manifest_file->length(context);
    if (!length.ok()) return publish_failure(before, length.error());
    if (length.value() != kManifestFileBytes) {
        return publish_failure(before, publish_error(
            PersistenceErrorCode::InvalidFormat,
            L"manifest file length is not exactly 8192 bytes"));
    }
    ManifestSlotBytes active_bytes{};
    const auto active_offset =
        static_cast<std::uint64_t>(request.active_slot.slot_index) *
        kManifestSlotBytes;
    auto read = request.manifest_file->read_exact(
        context, active_offset, active_bytes);
    if (!read.ok()) return publish_failure(before, read.error());
    const auto decoded_active = decode_manifest_slot(active_bytes);
    if (!decoded_active.ok() ||
        decoded_active.value() != request.active_slot) {
        return publish_failure(before, publish_error(
            PersistenceErrorCode::GenerationConflict,
            L"active manifest slot identity changed before publication"));
    }

#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            ManifestPublishTestPhase::BeforeSlotWrite, before)) {
        return *failure;
    }
#endif
    auto stage = lease.note_publish_attempt_started();
    if (!stage.ok()) return publish_failure(before, stage.error());
    auto written = request.manifest_file->write_exact(
        request.candidate.file_offset, request.candidate.bytes);
    if (!written.ok()) {
        return publish_failure(
            ManifestPublishBoundary::SlotWriteAttempted, written.error());
    }
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            ManifestPublishTestPhase::AfterSlotWrite,
            ManifestPublishBoundary::SlotWriteAttempted)) {
        return *failure;
    }
    if (const auto failure = injected(
            ManifestPublishTestPhase::BeforeFlush,
            ManifestPublishBoundary::ManifestFlushAttempted)) {
        return *failure;
    }
#endif
    auto flushed = request.manifest_file->flush();
    if (!flushed.ok()) {
        return publish_failure(
            ManifestPublishBoundary::ManifestFlushAttempted,
            flushed.error());
    }
    stage = lease.note_durable_commit_proven();
    if (!stage.ok()) {
        return publish_failure(
            ManifestPublishBoundary::ManifestFlushSucceeded,
            stage.error());
    }
    if (request.record_durable_commit) {
        try {
            stage = request.record_durable_commit();
        } catch (...) {
            stage = Result<void>::failure(publish_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"durable commit observer threw an exception"));
        }
        if (!stage.ok()) {
            return publish_failure(
                ManifestPublishBoundary::ManifestFlushSucceeded,
                stage.error());
        }
    }
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            ManifestPublishTestPhase::AfterFlush,
            ManifestPublishBoundary::ManifestFlushSucceeded)) {
        return *failure;
    }
    if (const auto failure = injected(
            ManifestPublishTestPhase::BeforeReadback,
            ManifestPublishBoundary::PostCommitReadback)) {
        return *failure;
    }
#endif

    ManifestSlotBytes readback{};
    read = request.manifest_file->read_exact(
        request.candidate.file_offset, readback);
    if (!read.ok()) {
        return publish_failure(
            ManifestPublishBoundary::PostCommitReadback, read.error());
    }
    const auto decoded = decode_manifest_slot(readback);
    if (readback != request.candidate.bytes || !decoded.ok() ||
        !same_durable_manifest(decoded.value(), request.candidate.slot)) {
        return publish_failure(
            ManifestPublishBoundary::PostCommitReadback,
            publish_error(PersistenceErrorCode::VerificationFailed,
                          L"published manifest slot readback differs"));
    }
    Result<void> validated = [&]() {
        try {
#if defined(MYRD_ENABLE_TEST_SEAMS)
            if (request.test_validate_candidate) {
                return request.test_validate_candidate(
                    request.candidate.slot, readback);
            }
#endif
            return validate_complete_candidate_from_disk(
                *request.complete_validation,
                request.candidate.slot);
        } catch (...) {
            return Result<void>::failure(publish_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"candidate validator threw an exception"));
        }
    }();
    if (!validated.ok()) {
        return publish_failure(
            ManifestPublishBoundary::PostCommitValidation,
            validated.error());
    }
#if defined(MYRD_ENABLE_TEST_SEAMS)
    if (const auto failure = injected(
            ManifestPublishTestPhase::AfterReadback,
            ManifestPublishBoundary::PostCommitValidation)) {
        return *failure;
    }
#endif
    return {
        ManifestPublishBoundary::PostCommitValidation,
        true,
        std::nullopt};
}

namespace {

using RecoveryCallback = std::function<Result<CommitRecoveryEvidence>(
    const OperationContext&)>;
using ReflushCallback = std::function<Result<void>(
    const OperationContext&, const ManifestSlot&)>;

CommitResolutionOutcome resolve_indeterminate_commit_impl(
    const OperationContext& context,
    PublishOperationLease& lease,
    const CheckpointGeneration old_generation,
    const CheckpointGeneration attempted_generation,
    const std::uint8_t attempted_slot_index,
    const RecoveryCallback& recover,
    const ReflushCallback& reflush_manifest) {
    CommitResolutionOutcome outcome{};
    auto fail = [&](const CommitDisposition disposition,
                    Error error) {
        outcome.disposition = disposition;
        outcome.error = std::move(error);
        return outcome;
    };
    if (!recover || !reflush_manifest ||
        old_generation >= kInvalidGeneration - 1 ||
        attempted_generation != old_generation + 1 ||
        attempted_slot_index > 1) {
        return fail(CommitDisposition::CommitIndeterminate, publish_error(
            PersistenceErrorCode::InvalidFormat,
            L"commit resolution request is incomplete"));
    }
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return fail(CommitDisposition::CommitIndeterminate, live.error());
    }
    auto evidence = [&]() -> Result<CommitRecoveryEvidence> {
        try {
            return recover(context);
        } catch (...) {
            return Result<CommitRecoveryEvidence>::failure(publish_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"commit recovery probe threw an exception"));
        }
    }();
    if (!evidence.ok()) {
        return fail(CommitDisposition::CommitIndeterminate, evidence.error());
    }
    switch (evidence.value().finding) {
    case CommitRecoveryFinding::NewGenerationComplete:
        if (!evidence.value().complete_manifest.has_value() ||
            evidence.value().attempted_slot_provably_invalid ||
            evidence.value().complete_manifest->slot_index !=
                attempted_slot_index ||
            evidence.value().complete_manifest->generation
                    .checkpoint_generation !=
                attempted_generation) {
            return fail(CommitDisposition::CommitIndeterminate, publish_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"new-generation recovery evidence has no manifest"));
        }
        outcome.recovery_evidence_complete = true;
        live = lease.note_publish_attempt_started();
        if (!live.ok()) {
            return fail(CommitDisposition::CommitIndeterminate,
                        live.error());
        }
        try {
            live = reflush_manifest(
                context, *evidence.value().complete_manifest);
        } catch (...) {
            live = Result<void>::failure(publish_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"manifest reflusher threw an exception"));
        }
        if (!live.ok()) {
            // The resolution attempt has crossed its own publication
            // boundary, but durable knowledge remains indeterminate until a
            // later recovery pass proves it.
            return fail(CommitDisposition::CommitIndeterminate,
                        live.error());
        }
        live = lease.note_durable_commit_proven();
        if (!live.ok()) {
            return fail(CommitDisposition::CommitIndeterminate,
                        live.error());
        }
        outcome.manifest_reflushed = true;
        outcome.disposition = CommitDisposition::CommittedNeedsReconciliation;
        return outcome;
    case CommitRecoveryFinding::OldGenerationOnly:
        if (!evidence.value().complete_manifest.has_value() ||
            !evidence.value().attempted_slot_provably_invalid ||
            evidence.value().complete_manifest->slot_index ==
                attempted_slot_index ||
            evidence.value().complete_manifest->generation
                    .checkpoint_generation != old_generation) {
            return fail(CommitDisposition::CommitIndeterminate, publish_error(
                PersistenceErrorCode::CommitIndeterminate,
                L"old-generation evidence does not prove attempted slot invalid"));
        }
        outcome.recovery_evidence_complete = true;
        outcome.disposition = CommitDisposition::NotCommitted;
        return outcome;
    case CommitRecoveryFinding::Ambiguous:
        return fail(CommitDisposition::CommitIndeterminate, publish_error(
            PersistenceErrorCode::CommitIndeterminate,
            L"recovery cannot prove old or new complete generation"));
    }
    return fail(CommitDisposition::CommitIndeterminate, publish_error(
        PersistenceErrorCode::FatalInternalInvariant,
        L"unknown commit recovery finding"));
}

}  // namespace

#if defined(MYRD_ENABLE_TEST_SEAMS)
FrozenPublicationIdentity make_frozen_publication_identity_for_test(
    const CheckpointGeneration old_generation,
    const CheckpointGeneration attempted_generation,
    const std::uint8_t attempted_slot_index) {
    return FrozenPublicationIdentity{
        old_generation, attempted_generation, attempted_slot_index};
}

CommitResolutionOutcome resolve_indeterminate_commit(
    const OperationContext& context,
    PublishOperationLease& lease,
    const CommitResolutionAttemptRequest& request) {
    return resolve_indeterminate_commit_impl(
        context, lease, request.old_generation,
        request.attempted_generation, request.attempted_slot_index,
        request.recover, request.reflush_manifest);
}
#endif

CommitResolutionOutcome resolve_indeterminate_commit_from_disk(
    const OperationContext& context,
    PublishOperationLease& lease,
    const FilesystemCommitResolution& filesystem,
    const FrozenPublicationIdentity& identity) {
    RecoveryCallback recover = [&](const OperationContext& probe_context) {
        return recover_commit_evidence_from_disk(
            probe_context, filesystem.bundle_root,
            filesystem.manifest_relative_name, filesystem.options,
            identity);
    };
    ReflushCallback reflush = [&](const OperationContext& reflush_context,
                                  const ManifestSlot& manifest) {
        if (filesystem.writer_lock == nullptr) {
            return Result<void>::failure(publish_error(
                PersistenceErrorCode::InvalidFormat,
                L"commit resolution has no writer capability"));
        }
        return reflush_complete_manifest(
            reflush_context, *filesystem.writer_lock, manifest);
    };
    return resolve_indeterminate_commit_impl(
        context, lease, identity.old_generation(),
        identity.attempted_generation(), identity.attempted_slot_index(),
        recover, reflush);
}

Result<CommitRecoveryEvidence> recover_commit_evidence_from_disk(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const std::string_view manifest_relative_name,
    const RecoveryOptions& options,
    const FrozenPublicationIdentity& identity) {
    const auto old_generation = identity.old_generation();
    const auto attempted_generation = identity.attempted_generation();
    const auto attempted_slot_index = identity.attempted_slot_index();
    if (old_generation >= kInvalidGeneration - 1 ||
        attempted_generation != old_generation + 1 ||
        attempted_slot_index > 1) {
        return Result<CommitRecoveryEvidence>::failure(publish_error(
            PersistenceErrorCode::InvalidFormat,
            L"commit recovery generation identity is invalid"));
    }
    const auto recovered = recover_complete_image(
        context, bundle_root, manifest_relative_name, options);
    if (!recovered.ok()) {
        if (recovered.error.has_value()) {
            return Result<CommitRecoveryEvidence>::failure(
                *recovered.error);
        }
        return Result<CommitRecoveryEvidence>::success({
            CommitRecoveryFinding::Ambiguous, std::nullopt, false});
    }
    const auto& image = *recovered.image;
    if (image.checkpoint_generation == attempted_generation &&
        image.selected_slot == attempted_slot_index &&
        image.selected_manifest.slot_index == attempted_slot_index) {
        return Result<CommitRecoveryEvidence>::success({
            CommitRecoveryFinding::NewGenerationComplete,
            image.selected_manifest, false});
    }
    if (image.checkpoint_generation != old_generation ||
        image.selected_slot == attempted_slot_index) {
        return Result<CommitRecoveryEvidence>::success({
            CommitRecoveryFinding::Ambiguous, std::nullopt, false});
    }

    const auto& attempted = image.diagnostics[attempted_slot_index];
    const bool deterministic_invalidity =
        attempted.status == ManifestCandidateCheckStatus::Corrupt ||
        attempted.status == ManifestCandidateCheckStatus::Unsupported ||
        attempted.status == ManifestCandidateCheckStatus::Conflict;
    if (!deterministic_invalidity ||
        !attempted.first_failed_check.has_value()) {
        // Missing, access, cancellation and I/O evidence are not proof that
        // the attempted slot is invalid.
        return Result<CommitRecoveryEvidence>::success({
            CommitRecoveryFinding::Ambiguous, std::nullopt, false});
    }
    return Result<CommitRecoveryEvidence>::success({
        CommitRecoveryFinding::OldGenerationOnly,
        image.selected_manifest, true});
}

Result<void> reflush_complete_manifest(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    const ManifestSlot& recovered_manifest) {
    auto live = check_operation_context(context);
    if (!live.ok()) return live;
    if (recovered_manifest.slot_index > 1 ||
        !validate_manifest_slot_semantics(recovered_manifest).ok()) {
        return Result<void>::failure(publish_error(
            PersistenceErrorCode::InvalidFormat,
            L"recovered manifest candidate is invalid"));
    }
    auto capability = open_locked_manifest_for_mutation(writer_lock);
    if (!capability.ok()) {
        return Result<void>::failure(capability.error());
    }
    auto file = open_validated_persistence_file(capability.move_value());
    if (!file.ok()) return Result<void>::failure(file.error());

    // Resolution has already entered its uncancellable publication attempt.
    // Verify the reopened object using uncancellable exact I/O, then flush the
    // same object. The normal recovery pass above remains the authority for
    // referenced base/journal completeness.
    const auto length = file.value()->length();
    if (!length.ok()) return Result<void>::failure(length.error());
    if (length.value() != kManifestFileBytes) {
        return Result<void>::failure(publish_error(
            PersistenceErrorCode::ManifestCorrupt,
            L"reopened manifest length is not exactly 8192 bytes"));
    }
    ManifestSlotBytes bytes{};
    auto read = file.value()->read_exact(
        static_cast<std::uint64_t>(recovered_manifest.slot_index) *
            kManifestSlotBytes,
        bytes);
    if (!read.ok()) return read;
    const auto canonical = encode_manifest_slot(recovered_manifest);
    const auto decoded = decode_manifest_slot(bytes);
    if (!canonical.ok() || canonical.value() != bytes || !decoded.ok() ||
        decoded.value() != recovered_manifest) {
        return Result<void>::failure(publish_error(
            PersistenceErrorCode::GenerationConflict,
            L"reopened manifest no longer contains recovered candidate"));
    }
    // Do not turn a close failure after this flush into an indeterminate
    // result: successful flush is the resolver's durable proof point.
    return file.value()->flush();
}

Result<void> validate_complete_candidate_from_disk(
    const CompleteCandidateValidation& validation,
    const ManifestSlot& expected_candidate) {
    if (validation.bundle_root.empty() ||
        validation.manifest_relative_name.empty() ||
        validate_limits(validation.options.limits) != LimitsStatus::Ok) {
        return Result<void>::failure(publish_error(
            PersistenceErrorCode::InvalidFormat,
            L"complete candidate validation request is invalid"));
    }
    // The manifest commit point is already durable. Ordinary cancellation or
    // a caller deadline can no longer turn it back into a pre-commit result.
    const auto recovered = recover_complete_image(
        OperationContext{}, validation.bundle_root,
        validation.manifest_relative_name, validation.options);
    if (!recovered.ok() || !recovered.image.has_value()) {
        return Result<void>::failure(recovered.error.value_or(publish_error(
            PersistenceErrorCode::VerificationFailed,
            L"normal recovery did not produce a complete candidate")));
    }
    const auto& image = *recovered.image;
    if (image.selected_slot != expected_candidate.slot_index ||
        image.checkpoint_generation !=
            expected_candidate.generation.checkpoint_generation ||
        image.selected_manifest != expected_candidate) {
        return Result<void>::failure(publish_error(
            PersistenceErrorCode::GenerationConflict,
            L"normal recovery selected a different manifest candidate"));
    }
    return Result<void>::success();
}

}  // namespace myrd::persistence
