#pragma once

#include "src/common/result.h"
#include "src/persistence/file_io.h"
#include "src/persistence/image_session.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/manifest_format.h"
#include "src/persistence/persistence_types.h"
#include "src/persistence/recovery_engine.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace myrd::persistence {

class BundleWriterLock;
class PreparedCheckpointTransaction;
class ValidatedBundleFile;

// Monotonic knowledge boundary for one manifest publication attempt. The
// exact slot write changes knowledge from safely-not-committed to unknown;
// successful manifest flush is the unique durable commit point.
enum class ManifestPublishBoundary : std::uint8_t {
    BeforeSlotWriteAttempt = 0,
    SlotWriteAttempted = 1,
    ManifestFlushAttempted = 2,
    ManifestFlushSucceeded = 3,
    PostCommitReadback = 4,
    PostCommitValidation = 5,
};

enum class ManifestKillRecovery : std::uint8_t {
    OldCompleteGenerationOnly = 0,
    OldOrNewCompleteGeneration,
    NewCompleteGenerationOnly,
};

struct PreparedManifestCandidate final {
    ManifestSlot slot{};
    ManifestSlotBytes bytes{};
    std::uint64_t file_offset{};
};

// Compaction replaces both immutable artifacts while publishing the ordinary
// G -> G+1 checkpoint commit. Checkpoint publication leaves this absent and
// therefore retains the active slot's base and journal names/identities.
struct ManifestArtifactReplacement final {
    ManifestSlot source_active_slot{};
    BaseGeneration base_generation{};
    std::string base_relative_name;
    VhdxFileIdentity base_identity{};
    WindowsFileIdentity base_windows_identity{};
    std::uint64_t base_expected_length{};
    // Pins the verified object against write/delete replacement until the
    // transaction has published and reconciled the new immutable view.
    std::shared_ptr<const ValidatedBundleFile> base_capability_anchor;
    std::string journal_relative_name;
    JournalFileIdentity journal_identity{};
    JournalEpoch journal_epoch{};
};

struct CompleteCandidateValidation final {
    std::filesystem::path bundle_root{};
    std::string manifest_relative_name{};
    RecoveryOptions options{};
};

#if defined(MYRD_ENABLE_TEST_SEAMS)
using ManifestCandidateValidator = std::function<Result<void>(
    const ManifestSlot&, std::span<const std::byte>)>;
enum class ManifestPublishTestPhase : std::uint8_t {
    BeforeSlotWrite = 0,
    AfterSlotWrite,
    BeforeFlush,
    AfterFlush,
    BeforeReadback,
    AfterReadback,
};
using ManifestPublishTestPhaseHook =
    std::function<Result<void>(ManifestPublishTestPhase)>;
#endif

struct ManifestPublishRequest final {
    std::shared_ptr<IPersistenceFile> manifest_file{};
    ManifestSlot active_slot{};
    PreparedManifestCandidate candidate{};
    std::optional<CompleteCandidateValidation> complete_validation{};
    std::function<Result<void>()> record_durable_commit{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    ManifestCandidateValidator test_validate_candidate{};
    ManifestPublishTestPhaseHook test_phase_hook{};
#endif
};

struct CheckpointManifestPublishRequest final {
    std::unique_ptr<IPersistenceFile> manifest_file{};
    ManifestSlot active_slot{};
    std::uint64_t committed_time_100ns{};
    std::optional<CompleteCandidateValidation> complete_validation{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    ManifestCandidateValidator test_validate_candidate{};
    ManifestPublishTestPhaseHook test_phase_hook{};
#endif
};

struct CheckpointTransactionOutcome final {
    CommitDisposition disposition{CommitDisposition::NotCommitted};
    CheckpointGeneration generation{};
    bool manifest_candidate_verified{};
    std::optional<Error> error{};
    std::optional<PersistenceErrorContext> error_context{};

    [[nodiscard]] bool ok() const noexcept {
        return disposition == CommitDisposition::Committed &&
               manifest_candidate_verified && !error.has_value();
    }
};

struct ManifestPublishAttemptOutcome final {
    ManifestPublishBoundary boundary{
        ManifestPublishBoundary::BeforeSlotWriteAttempt};
    bool candidate_verified{};
    std::optional<Error> error{};

    [[nodiscard]] bool ok() const noexcept {
        return !error.has_value() && candidate_verified &&
               static_cast<std::uint8_t>(boundary) >=
                   static_cast<std::uint8_t>(
                       ManifestPublishBoundary::ManifestFlushSucceeded);
    }
};

enum class CommitRecoveryFinding : std::uint8_t {
    NewGenerationComplete = 0,
    OldGenerationOnly,
    Ambiguous,
};

// A resolver must obtain this fact from closed/reopened handles and the normal
// recovery engine. It is deliberately not constructible from cached slot
// bytes in the production adapter.
struct CommitRecoveryEvidence final {
    CommitRecoveryFinding finding{CommitRecoveryFinding::Ambiguous};
    std::optional<ManifestSlot> complete_manifest{};
    bool attempted_slot_provably_invalid{};
};

// Unforgeable authority for the exact publication attempt being resolved.
// Only PreparedCheckpointTransaction can create production instances, after
// it has prepared the exact inactive slot. Move-only ownership prevents a
// caller from synthesizing old/new generation or slot identity at a lower
// resolver entry point.
class FrozenPublicationIdentity final {
public:
    FrozenPublicationIdentity() = delete;
    FrozenPublicationIdentity(const FrozenPublicationIdentity&) = delete;
    FrozenPublicationIdentity& operator=(
        const FrozenPublicationIdentity&) = delete;
    FrozenPublicationIdentity(FrozenPublicationIdentity&&) noexcept = default;
    FrozenPublicationIdentity& operator=(
        FrozenPublicationIdentity&&) noexcept = default;

    [[nodiscard]] CheckpointGeneration old_generation() const noexcept {
        return old_generation_;
    }
    [[nodiscard]] CheckpointGeneration attempted_generation() const noexcept {
        return attempted_generation_;
    }
    [[nodiscard]] std::uint8_t attempted_slot_index() const noexcept {
        return attempted_slot_index_;
    }

private:
    friend class PreparedCheckpointTransaction;
#if defined(MYRD_ENABLE_TEST_SEAMS)
    friend FrozenPublicationIdentity
    make_frozen_publication_identity_for_test(
        CheckpointGeneration,
        CheckpointGeneration,
        std::uint8_t);
#endif

    FrozenPublicationIdentity(
        const CheckpointGeneration old_generation,
        const CheckpointGeneration attempted_generation,
        const std::uint8_t attempted_slot_index) noexcept
        : old_generation_(old_generation),
          attempted_generation_(attempted_generation),
          attempted_slot_index_(attempted_slot_index) {}

    CheckpointGeneration old_generation_{};
    CheckpointGeneration attempted_generation_{};
    std::uint8_t attempted_slot_index_{};
};

#if defined(MYRD_ENABLE_TEST_SEAMS)
[[nodiscard]] FrozenPublicationIdentity
make_frozen_publication_identity_for_test(
    CheckpointGeneration old_generation,
    CheckpointGeneration attempted_generation,
    std::uint8_t attempted_slot_index);
#endif

#if defined(MYRD_ENABLE_TEST_SEAMS)
using CommitRecoveryProbe = std::function<Result<CommitRecoveryEvidence>(
    const OperationContext&)>;
using CommitManifestReflusher = std::function<Result<void>(
    const OperationContext&, const ManifestSlot&)>;
struct CommitResolutionAttemptRequest final {
    CheckpointGeneration old_generation{};
    CheckpointGeneration attempted_generation{};
    std::uint8_t attempted_slot_index{};
    CommitRecoveryProbe recover{};
    CommitManifestReflusher reflush_manifest{};
};
#endif

struct FilesystemCommitResolution final {
    std::filesystem::path bundle_root{};
    std::string manifest_relative_name{};
    RecoveryOptions options{};
    BundleWriterLock* writer_lock{};
};

struct CommitResolutionRequest final {
    std::optional<FilesystemCommitResolution> filesystem{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    CommitRecoveryProbe test_recover{};
    CommitManifestReflusher test_reflush_manifest{};
#endif
};

struct CommitResolutionOutcome final {
    CommitDisposition disposition{CommitDisposition::CommitIndeterminate};
    bool recovery_evidence_complete{};
    bool manifest_reflushed{};
    std::optional<Error> error{};
};

#if defined(MYRD_ENABLE_TEST_SEAMS)
[[nodiscard]] CommitResolutionOutcome resolve_indeterminate_commit(
    const OperationContext& context,
    PublishOperationLease& lease,
    const CommitResolutionAttemptRequest& request);
#endif

[[nodiscard]] CommitResolutionOutcome resolve_indeterminate_commit_from_disk(
    const OperationContext& context,
    PublishOperationLease& lease,
    const FilesystemCommitResolution& filesystem,
    const FrozenPublicationIdentity& identity);

// Product resolver adapters. The recovery function opens fresh inspection
// handles through recover_complete_image(); the reflush function opens a new
// writer-authorized manifest handle and never rewrites either slot.
[[nodiscard]] Result<CommitRecoveryEvidence>
recover_commit_evidence_from_disk(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    std::string_view manifest_relative_name,
    const RecoveryOptions& options,
    const FrozenPublicationIdentity& identity);

[[nodiscard]] Result<void> reflush_complete_manifest(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    const ManifestSlot& recovered_manifest);

[[nodiscard]] Result<void> validate_complete_candidate_from_disk(
    const CompleteCandidateValidation& validation,
    const ManifestSlot& expected_candidate);

[[nodiscard]] Result<PreparedManifestCandidate> prepare_manifest_candidate(
    const ManifestSlot& active_slot,
    const PreparedCheckpoint& checkpoint,
    std::uint64_t committed_time_100ns,
    const std::optional<ManifestArtifactReplacement>& replacement =
        std::nullopt);

[[nodiscard]] ManifestPublishAttemptOutcome attempt_manifest_publication(
    const OperationContext& context,
    PublishOperationLease& lease,
    const ManifestPublishRequest& request);

[[nodiscard]] constexpr CommitDisposition classify_manifest_publish_failure(
    const ManifestPublishBoundary boundary) noexcept {
    switch (boundary) {
    case ManifestPublishBoundary::BeforeSlotWriteAttempt:
        return CommitDisposition::NotCommitted;
    case ManifestPublishBoundary::SlotWriteAttempted:
    case ManifestPublishBoundary::ManifestFlushAttempted:
        return CommitDisposition::CommitIndeterminate;
    case ManifestPublishBoundary::ManifestFlushSucceeded:
    case ManifestPublishBoundary::PostCommitReadback:
    case ManifestPublishBoundary::PostCommitValidation:
        return CommitDisposition::CommittedNeedsReconciliation;
    }
    return CommitDisposition::CommittedNeedsReconciliation;
}

[[nodiscard]] constexpr bool manifest_commit_proven(
    const ManifestPublishBoundary boundary) noexcept {
    switch (boundary) {
    case ManifestPublishBoundary::BeforeSlotWriteAttempt:
    case ManifestPublishBoundary::SlotWriteAttempted:
    case ManifestPublishBoundary::ManifestFlushAttempted:
        return false;
    case ManifestPublishBoundary::ManifestFlushSucceeded:
    case ManifestPublishBoundary::PostCommitReadback:
    case ManifestPublishBoundary::PostCommitValidation:
        return true;
    }
    return false;
}

[[nodiscard]] constexpr bool manifest_publish_cancellation_permitted(
    const ManifestPublishBoundary boundary) noexcept {
    return boundary == ManifestPublishBoundary::BeforeSlotWriteAttempt;
}

[[nodiscard]] constexpr ManifestKillRecovery
manifest_kill_recovery_set(
    const ManifestPublishBoundary boundary) noexcept {
    switch (boundary) {
    case ManifestPublishBoundary::BeforeSlotWriteAttempt:
        return ManifestKillRecovery::OldCompleteGenerationOnly;
    case ManifestPublishBoundary::SlotWriteAttempted:
    case ManifestPublishBoundary::ManifestFlushAttempted:
        return ManifestKillRecovery::OldOrNewCompleteGeneration;
    case ManifestPublishBoundary::ManifestFlushSucceeded:
    case ManifestPublishBoundary::PostCommitReadback:
    case ManifestPublishBoundary::PostCommitValidation:
        return ManifestKillRecovery::NewCompleteGenerationOnly;
    }
    return ManifestKillRecovery::OldOrNewCompleteGeneration;
}

}  // namespace myrd::persistence
