#pragma once

#include "src/common/result.h"
#include "src/common/operation_context.h"
#include "src/persistence/composite_backing_store.h"
#include "src/persistence/manifest_format.h"
#include "src/persistence/recovery_engine.h"
#include "src/storage/dynamic_block_store.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace myrd::persistence {

class BundleWriterLock;
class PublishOperationLease;

enum class ImmutableArtifactKind : std::uint8_t {
    BaseVhdx,
    Journal,
};

// An entry may be supplied to cleanup only after the caller has validated its
// format identity while holding this exact Windows file object. Unknown files
// discovered by a directory scan are deliberately not owned artifacts.
struct OwnedImmutableArtifact final {
    ImmutableArtifactKind kind{ImmutableArtifactKind::BaseVhdx};
    std::string relative_name{};
    WindowsFileIdentity windows_identity{};
    ImageUuid image_uuid{};
    VhdxFileIdentity base_identity{};
    std::array<std::byte, 16> virtual_disk_id{};
    JournalFileIdentity journal_identity{};
    JournalEpoch journal_epoch{};
};

struct RetentionLiveViewReference final {
    CompositeBackingIdentity identity{};
    std::weak_ptr<const IBackingStore> lifetime{};
};

// Pure-decision input and test oracle. The production deletion API does not
// accept this caller-constructible structure: it captures its own reference
// set from the writer-locked manifest and normal recovery while holding the
// cleanup gate.
struct RetentionReferenceSnapshot final {
    ImageUuid image_uuid{};
    std::array<std::byte, 16> virtual_disk_id{};
    std::array<std::optional<ManifestSlot>, 2> valid_slots{};
    std::vector<RetentionLiveViewReference> live_views{};
    std::vector<OwnedImmutableArtifact> in_flight_artifacts{};
};

enum class RetentionDecision : std::uint8_t {
    Eligible,
    InvalidName,
    ReparsePoint,
    HardLinked,
    UnknownOwnership,
    ManifestReferenced,
    LiveViewReferenced,
    InFlightReferenced,
};

[[nodiscard]] const char* to_string(RetentionDecision decision) noexcept;

[[nodiscard]] Result<RetentionLiveViewReference>
make_retention_live_view_reference(const BackingView& view);

// Pure, non-mutating decision. Eligible is permission to proceed to a fresh
// handle-anchored format/identity validation, never permission to delete by
// pathname alone.
[[nodiscard]] RetentionDecision evaluate_retention_candidate(
    const OwnedImmutableArtifact& candidate,
    const RetentionReferenceSnapshot& references) noexcept;

enum class RetentionCleanupAction : std::uint8_t {
    Kept,
    Deleted,
};

struct RetentionCleanupEntry final {
    ImmutableArtifactKind kind{ImmutableArtifactKind::BaseVhdx};
    std::string relative_name{};
    RetentionDecision decision{RetentionDecision::UnknownOwnership};
    RetentionCleanupAction action{RetentionCleanupAction::Kept};
    bool delete_attempted{};
    ErrorDomain error_domain{ErrorDomain::internal};
    std::uint32_t error_code{};
    std::uint32_t native_code{};
    bool has_error{};
};

struct RetentionCleanupReport final {
    bool dry_run{};
    std::uint64_t candidate_count{};
    std::uint64_t eligible_count{};
    std::uint64_t delete_attempt_count{};
    std::uint64_t deleted_count{};
    std::vector<RetentionCleanupEntry> entries{};
};

#if defined(MYRD_ENABLE_TEST_SEAMS)
enum class RetentionCleanupTestPhase : std::uint8_t {
    AfterProtectionBeforeValidation,
    BeforeDelete,
};
#endif

struct RetentionCleanupOptions final {
    std::uint64_t max_candidates{};
    bool dry_run{};
    std::size_t max_live_views{};
    RecoveryOptions recovery{};
#if defined(MYRD_ENABLE_TEST_SEAMS)
    std::function<Result<void>(RetentionCleanupTestPhase)> phase_hook{};
#endif
};

// Candidates are explicit validated ownership records. The supplied store is
// the runtime authority: its current composite identity must equal normal
// recovery, and every still-live retired backing view is protected. This
// function never enumerates a directory, expands a glob, follows a reparse
// point or performs recursive deletion. Per-file sharing/validation/delete
// failures are harmless retention and do not invalidate the current generation.
[[nodiscard]] Result<RetentionCleanupReport> cleanup_retention_artifacts(
    const OperationContext& context,
    PublishOperationLease cleanup_lease,
    BundleWriterLock& writer,
    DynamicBlockStore& store,
    const std::filesystem::path& bundle_root,
    std::string_view manifest_relative_name,
    std::span<const OwnedImmutableArtifact> candidates,
    RetentionCleanupOptions options);

#if defined(MYRD_ENABLE_TEST_SEAMS)
namespace test {
// Narrow test seam for fault and reference-oracle cases. It is absent from the
// BUILD_TESTING=OFF product library.
[[nodiscard]] Result<RetentionCleanupReport>
cleanup_retention_artifacts_with_snapshot(
    const OperationContext& context,
    PublishOperationLease cleanup_lease,
    BundleWriterLock& writer,
    const std::filesystem::path& bundle_root,
    const RetentionReferenceSnapshot& references,
    std::span<const OwnedImmutableArtifact> candidates,
    RetentionCleanupOptions options);
} // namespace test
#endif

} // namespace myrd::persistence
