#include "image_inspector.h"

#include "src/common/sha256_stream.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/persistence_types.h"
#include "src/persistence/vhdx_reader.h"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <span>
#include <vector>

namespace myrd::persistence {
namespace {

Error inspection_error(PersistenceErrorCode code, const wchar_t* message) {
    return {ErrorDomain::persistence, static_cast<std::uint32_t>(code),
            L"inspect_image", message};
}

bool is_expected_rejection(const Error& error) noexcept {
    if (error.domain != ErrorDomain::persistence) return false;
    switch (static_cast<PersistenceErrorCode>(error.code)) {
    case PersistenceErrorCode::InvalidFormat:
    case PersistenceErrorCode::UnsupportedVhdx:
    case PersistenceErrorCode::ManifestCorrupt:
    case PersistenceErrorCode::JournalCorrupt:
    case PersistenceErrorCode::BackingDataCorrupt:
    case PersistenceErrorCode::GenerationConflict:
    case PersistenceErrorCode::VerificationFailed:
        return true;
    default:
        return false;
    }
}

ImageInspection make_inspection(const RecoveryOutcome& outcome) {
    ImageInspection inspection{};
    inspection.diagnostics = outcome.diagnostics;
    if (outcome.image.has_value()) {
        const auto& image = *outcome.image;
        inspection.selected = SelectedImageInspection{
            image.geometry, image.image_uuid, image.checkpoint_generation,
            image.base_generation, image.journal_epoch, image.selected_slot,
            image.fallback, image.backing_identity};
    } else {
        inspection.rejection = outcome.error;
    }
    return inspection;
}

void encode_u64(std::uint64_t value,
                std::span<std::byte, sizeof(std::uint64_t)> output) noexcept {
    for (std::size_t index = 0; index < output.size(); ++index) {
        output[index] = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
    }
}

Result<void> update_hash(Sha256Stream& hash,
                         std::span<const std::byte> bytes) {
    auto result = hash.update(bytes);
    if (!result.ok()) return Result<void>::failure(result.error());
    return Result<void>::success();
}

bool add_would_overflow(std::uint64_t left, std::uint64_t right) noexcept {
    return left > (std::numeric_limits<std::uint64_t>::max)() - right;
}

Result<void> validate_options(const ImageInspectOptions& options) {
    if (!validate_image_component(options.manifest_relative_name).ok() ||
        validate_limits(options.recovery.limits) != LimitsStatus::Ok ||
        options.recovery.limits.max_bat_cache_bytes < kVhdxBatPageBytes ||
        options.recovery.limits.max_bat_cache_entries >
            options.recovery.limits.max_bat_cache_bytes /
                kVhdxBatPageBytes) {
        return Result<void>::failure(inspection_error(
            PersistenceErrorCode::InvalidFormat,
            L"inspection options are invalid"));
    }
    return Result<void>::success();
}

} // namespace

Result<ImageInspection> inspect_image(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const ImageInspectOptions& options) {
    auto valid = validate_options(options);
    if (!valid.ok()) return Result<ImageInspection>::failure(valid.error());
    auto outcome = recover_complete_image(
        context, bundle_root, options.manifest_relative_name,
        options.recovery);
    if (!outcome.ok() &&
        (!outcome.error.has_value() ||
         !is_expected_rejection(*outcome.error))) {
        return Result<ImageInspection>::failure(
            outcome.error.value_or(inspection_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"recovery returned neither an image nor an error")));
    }
    return Result<ImageInspection>::success(make_inspection(outcome));
}

namespace {

// One bit per condition that must hold for the candidate to be proved.  The
// mask is built with |= rather than early-exit so a caller that has to explain
// a refusal can say which conditions held and which did not.
std::uint32_t candidate_mismatch_mask(
    const CheckpointRecoveryInspection& observed,
    const CheckpointRecoveryCorrelation& expected) noexcept {
    std::uint32_t mask{};
    if (!(observed.image_uuid == expected.image_uuid)) {
        mask |= static_cast<std::uint32_t>(CheckpointRecoveryMismatch::ImageUuid);
    }
    // No prior-generation clause: the bundle records the *base* generation, not the
    // checkpoint the candidate was cut from, and the two differ after the first
    // checkpoint.  The reasoning and the measurement that found it are in the enum's
    // comment in the header.
    if (observed.candidate_generation != expected.candidate_generation) {
        mask |= static_cast<std::uint32_t>(
            CheckpointRecoveryMismatch::CandidateGeneration);
    }
    if (observed.journal_operation_id != expected.native_operation_id) {
        mask |= static_cast<std::uint32_t>(
            CheckpointRecoveryMismatch::NativeOperationId);
    }
    if (observed.selected_manifest_slot != expected.manifest_slot) {
        mask |= static_cast<std::uint32_t>(
            CheckpointRecoveryMismatch::ManifestSlot);
    }
    if (observed.selected_manifest_digest != expected.candidate_bytes_digest) {
        mask |= static_cast<std::uint32_t>(
            CheckpointRecoveryMismatch::ManifestDigest);
    }
    return mask;
}

} // namespace

Result<CheckpointRecoveryInspection> inspect_checkpoint(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const ImageInspectOptions& options,
    const CheckpointRecoveryCorrelation& expected) {
    auto valid = validate_options(options);
    if (!valid.ok()) {
        return Result<CheckpointRecoveryInspection>::failure(valid.error());
    }
    auto outcome = recover_complete_image(
        context, bundle_root, options.manifest_relative_name,
        options.recovery);
    if (!outcome.ok() &&
        (!outcome.error.has_value() ||
         !is_expected_rejection(*outcome.error))) {
        return Result<CheckpointRecoveryInspection>::failure(
            outcome.error.value_or(inspection_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"recovery returned neither an image nor an error")));
    }

    CheckpointRecoveryInspection inspection{};
    if (!outcome.image.has_value()) {
        // The bundle is readable but nothing in it is recoverable.  No candidate
        // condition can be verified against it, so the mask stays full and the
        // verdict stays unprovable: a corrupt or absent image must never be
        // read as "the checkpoint did not happen".
        inspection.candidate_mismatch_mask = kCheckpointCandidateConditions;
        inspection.verdict = CheckpointRecoveryVerdict::OwnershipUnprovable;
        return Result<CheckpointRecoveryInspection>::success(
            std::move(inspection));
    }

    const auto& image = *outcome.image;
    inspection.image_uuid = image.image_uuid;
    inspection.base_generation = image.base_generation;
    inspection.candidate_generation = image.checkpoint_generation;
    inspection.journal_operation_id = image.journal_operation_id;
    inspection.selected_manifest_slot = image.selected_slot;

    auto slot_bytes = read_manifest_slot_bytes(
        context, bundle_root, options.manifest_relative_name,
        image.selected_slot);
    if (!slot_bytes.ok()) {
        return Result<CheckpointRecoveryInspection>::failure(
            slot_bytes.error());
    }
    auto digest = sha256_digest(slot_bytes.value());
    if (!digest.ok()) {
        return Result<CheckpointRecoveryInspection>::failure(digest.error());
    }
    inspection.selected_manifest_digest = digest.value();

    inspection.candidate_mismatch_mask =
        candidate_mismatch_mask(inspection, expected);
    if (inspection.candidate_mismatch_mask == 0) {
        inspection.verdict = CheckpointRecoveryVerdict::CandidateGenerationProved;
    } else if (inspection.candidate_generation == expected.prior_generation &&
               inspection.journal_operation_id !=
                   expected.native_operation_id) {
        // The committed generation is the one the candidate was built on and
        // the journal's native id is somebody else's, so this operation
        // definitively did not commit.
        inspection.verdict = CheckpointRecoveryVerdict::PriorGenerationProved;
    } else {
        inspection.verdict = CheckpointRecoveryVerdict::OwnershipUnprovable;
    }
    return Result<CheckpointRecoveryInspection>::success(
        std::move(inspection));
}

Result<CompactionRecoveryInspection> inspect_compaction(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const ImageInspectOptions& options,
    const CompactionRecoveryCorrelation& expected) {
    auto valid = validate_options(options);
    if (!valid.ok()) {
        return Result<CompactionRecoveryInspection>::failure(valid.error());
    }
    auto outcome = recover_complete_image(
        context, bundle_root, options.manifest_relative_name,
        options.recovery);
    if (!outcome.ok() &&
        (!outcome.error.has_value() ||
         !is_expected_rejection(*outcome.error))) {
        return Result<CompactionRecoveryInspection>::failure(
            outcome.error.value_or(inspection_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"recovery returned neither an image nor an error")));
    }

    CompactionRecoveryInspection inspection{};
    if (!outcome.image.has_value()) {
        // Readable but nothing recoverable.  No generation can be attributed,
        // so the only honest verdict is the one that forbids both answers.
        inspection.verdict = CompactionRecoveryVerdict::Unprovable;
        return Result<CompactionRecoveryInspection>::success(
            std::move(inspection));
    }

    const auto& image = *outcome.image;
    inspection.image_uuid = image.image_uuid;
    inspection.observed_generation = image.checkpoint_generation;
    inspection.candidate_base_bytes =
        image.selected_manifest.base_expected_length;
    inspection.candidate_journal_bytes =
        image.backing_identity.journal_committed_length;
    if (!(image.image_uuid == expected.image_uuid)) {
        inspection.verdict = CompactionRecoveryVerdict::Unprovable;
        return Result<CompactionRecoveryInspection>::success(
            std::move(inspection));
    }
    if (image.checkpoint_generation == expected.source_generation) {
        inspection.verdict =
            CompactionRecoveryVerdict::SourceGenerationProved;
        return Result<CompactionRecoveryInspection>::success(
            std::move(inspection));
    }
    if (image.checkpoint_generation == expected.candidate_generation) {
        // A candidate whose cut epoch the service never recorded cannot be
        // confirmed: the new journal may contain post-cut writes that were
        // preserved, and without the cut there is no boundary to attribute them
        // to.  Fail closed as unprovable rather than promote a generation whose
        // provenance is incomplete.
        inspection.verdict = expected.cut_dirty_epoch_known
            ? CompactionRecoveryVerdict::CandidateGenerationProved
            : CompactionRecoveryVerdict::Unprovable;
        return Result<CompactionRecoveryInspection>::success(
            std::move(inspection));
    }
    inspection.verdict = CompactionRecoveryVerdict::Unprovable;
    return Result<CompactionRecoveryInspection>::success(
        std::move(inspection));
}

Result<ImageVerificationReport> verify_image(
    const OperationContext& context,
    const std::filesystem::path& bundle_root,
    const VerificationDepth depth,
    const ImageInspectOptions& options) {
    auto valid = validate_options(options);
    if (!valid.ok()) {
        return Result<ImageVerificationReport>::failure(valid.error());
    }
    if (depth != VerificationDepth::Structural &&
        depth != VerificationDepth::Full) {
        return Result<ImageVerificationReport>::failure(inspection_error(
            PersistenceErrorCode::InvalidFormat,
            L"verification depth is unknown"));
    }
    if (depth == VerificationDepth::Full &&
        options.recovery.limits.max_verify_batch_bytes < kM2BlockSize) {
        return Result<ImageVerificationReport>::failure(inspection_error(
            PersistenceErrorCode::InvalidFormat,
            L"full verification requires one bounded M2 block buffer"));
    }

    auto outcome = recover_complete_image(
        context, bundle_root, options.manifest_relative_name,
        options.recovery);
    if (!outcome.ok() &&
        (!outcome.error.has_value() ||
         !is_expected_rejection(*outcome.error))) {
        return Result<ImageVerificationReport>::failure(
            outcome.error.value_or(inspection_error(
                PersistenceErrorCode::FatalInternalInvariant,
                L"recovery returned neither an image nor an error")));
    }

    ImageVerificationReport report{};
    report.depth = depth;
    report.inspection = make_inspection(outcome);
    if (!outcome.image.has_value()) {
        report.failure = outcome.error;
        return Result<ImageVerificationReport>::success(std::move(report));
    }
    if (depth == VerificationDepth::Structural) {
        report.verified = true;
        return Result<ImageVerificationReport>::success(std::move(report));
    }
    if (outcome.image->fallback) {
        report.failure = inspection_error(
            PersistenceErrorCode::VerificationFailed,
            L"full verification refuses a fallback generation");
        return Result<ImageVerificationReport>::success(std::move(report));
    }

    const auto composite = std::dynamic_pointer_cast<
        const CompositeBackingStore>(outcome.image->backing_view.store);
    if (!composite || composite->identity() != outcome.image->backing_identity) {
        return Result<ImageVerificationReport>::failure(inspection_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"recovery did not return its validated composite view"));
    }

    std::vector<std::byte> block;
    try {
        block.resize(static_cast<std::size_t>(kM2BlockSize));
    } catch (const std::bad_alloc&) {
        return Result<ImageVerificationReport>::failure(inspection_error(
            PersistenceErrorCode::FatalInternalInvariant,
            L"verification buffer allocation failed"));
    }

    Sha256Stream hash;
    constexpr std::array<std::byte, 12> domain{
        std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
        std::byte{'-'}, std::byte{'V'}, std::byte{'E'}, std::byte{'R'},
        std::byte{'I'}, std::byte{'F'}, std::byte{'Y'}, std::byte{1}};
    auto initialized = update_hash(hash, domain);
    if (!initialized.ok()) {
        return Result<ImageVerificationReport>::failure(initialized.error());
    }
    std::array<std::byte, sizeof(std::uint64_t)> encoded_virtual_bytes{};
    encode_u64(outcome.image->geometry.virtual_bytes, encoded_virtual_bytes);
    initialized = update_hash(hash, encoded_virtual_bytes);
    if (!initialized.ok()) {
        return Result<ImageVerificationReport>::failure(initialized.error());
    }

    auto enumerated = composite->enumerate_candidate_blocks(
        context, [&](const std::uint64_t block_index) -> Result<void> {
            auto live = check_operation_context(context);
            if (!live.ok()) return live;
            if (block_index >
                (std::numeric_limits<std::uint64_t>::max)() / kM2BlockSize) {
                return Result<void>::failure(inspection_error(
                    PersistenceErrorCode::VerificationFailed,
                    L"candidate block offset overflows"));
            }
            const auto block_offset = block_index * kM2BlockSize;
            if (block_offset >= outcome.image->geometry.virtual_bytes) {
                return Result<void>::failure(inspection_error(
                    PersistenceErrorCode::VerificationFailed,
                    L"candidate block is outside image geometry"));
            }
            std::fill(block.begin(), block.end(), std::byte{});
            auto loaded = composite->load_block(context, block_index, block);
            if (!loaded.ok()) return Result<void>::failure(loaded.error());

            if (report.candidate_blocks ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                return Result<void>::failure(inspection_error(
                    PersistenceErrorCode::VerificationFailed,
                    L"candidate block count overflows"));
            }
            ++report.candidate_blocks;
            std::array<std::byte, sizeof(std::uint64_t)> encoded_index{};
            encode_u64(block_index, encoded_index);
            auto hashed = update_hash(hash, encoded_index);
            if (!hashed.ok()) return hashed;

            const auto valid_bytes = std::min<std::uint64_t>(
                kM2BlockSize,
                outcome.image->geometry.virtual_bytes - block_offset);
            std::array<std::byte, sizeof(std::uint64_t)> encoded_length{};
            encode_u64(valid_bytes, encoded_length);
            hashed = update_hash(hash, encoded_length);
            if (!hashed.ok()) return hashed;

            const std::array<std::byte, 1> kind{
                loaded.value().kind == BlockLoadKind::Data
                    ? std::byte{1} : std::byte{0}};
            hashed = update_hash(hash, kind);
            if (!hashed.ok()) return hashed;
            if (loaded.value().kind == BlockLoadKind::AllZero) {
                ++report.zero_blocks;
                return Result<void>::success();
            }
            ++report.data_blocks;
            if (add_would_overflow(report.data_bytes_hashed, valid_bytes)) {
                return Result<void>::failure(inspection_error(
                    PersistenceErrorCode::VerificationFailed,
                    L"verified byte count overflows"));
            }
            report.data_bytes_hashed += valid_bytes;
            return update_hash(hash, std::span<const std::byte>{block}.first(
                static_cast<std::size_t>(valid_bytes)));
        });
    if (!enumerated.ok()) {
        report.failure = enumerated.error();
        return Result<ImageVerificationReport>::success(std::move(report));
    }
    auto digest = hash.finalize();
    if (!digest.ok()) {
        return Result<ImageVerificationReport>::failure(digest.error());
    }
    report.logical_sha256 = digest.move_value();
    report.verified = true;
    return Result<ImageVerificationReport>::success(std::move(report));
}

} // namespace myrd::persistence
