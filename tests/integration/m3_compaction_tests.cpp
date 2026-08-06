#include "test_framework.h"

#include "src/persistence/compaction_coordinator.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/recovered_store_adapter.h"
#include "src/persistence/vhdx_reader.h"
#include "tests/support/fake_backing_store.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <filesystem>
#include <string_view>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

OperationContext bounded_context() {
    return OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(5)};
}

std::unique_ptr<DynamicBlockStore> make_live_store(
    const BackingGeneration generation = 7) {
    DynamicBlockStoreOptions options{
        2 * kM2BlockSize,
        3 * kM2BlockSize,
        kM2BlockSize,
        2 * kM2BlockSize,
        8,
        64,
        2 * kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250},
    };
    auto created = DynamicBlockStore::create({
        4 * kM2BlockSize,
        512,
        options,
        InitialBlockState::BackingUnresident,
        myrd::test::FakeBackingStoreBuilder(91).build(),
        generation,
    });
    REQUIRE(created.ok());
    return created.move_value();
}

class CompactionSandbox final {
public:
    CompactionSandbox() {
        static std::atomic<std::uint64_t> sequence{};
        root_ = std::filesystem::temp_directory_path() /
            ("myrd-m3-compaction-" +
             std::to_string(sequence.fetch_add(1)) + "-" +
             std::to_string(static_cast<std::uint64_t>(
                 std::chrono::steady_clock::now()
                     .time_since_epoch().count())));
        std::filesystem::create_directories(root_);
    }
    ~CompactionSandbox() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

GenerationZeroCreateRequest make_create_request(
    const std::filesystem::path& root) {
    constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
    GenerationZeroCreateRequest request{};
    request.bundle_root = root;
    request.manifest_relative_name = "disk.rdm";
    request.base_temporary_name = "base.create.tmp";
    request.base_relative_name = "base-0000.vhdx";
    request.journal_relative_name = "journal-e0001.rdj";
    request.geometry = {8 * kMiB, 512, 4096};
    request.image_uuid.bytes[0] = std::byte{0x11};
    request.base_identity.file_identity.file_write_guid[0] =
        std::byte{0x21};
    request.base_identity.file_identity.data_write_guid[0] =
        std::byte{0x22};
    request.base_identity.virtual_disk_id[0] = std::byte{0x23};
    request.journal_identity.journal_uuid[0] = std::byte{0x31};
    request.journal_epoch = 1;
    request.operation_id = 3501;
    request.created_time_100ns = 123456;
    request.vhdx_limits = {4096, 4096, kVhdxDataBlockBytes};
    request.recovery.limits = {
        64 * kMiB, 64 * kMiB, 8192,
        64 * kIndexNodeBytes, 64,
        64 * kVhdxBatPageBytes, 64,
        2 * kMiB, 2 * kMiB};
    return request;
}

DynamicBlockStoreOptions compaction_store_options() {
    return {
        4 * kM2BlockSize,
        8 * kM2BlockSize,
        2 * kM2BlockSize,
        2 * kM2BlockSize,
        16,
        64,
        4 * kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250},
    };
}

CompactionArtifactBuildRequest make_artifact_request(
    BundleWriterLock& writer,
    const RecoveredImage& image,
    std::uint64_t& offline_validation_calls) {
    CompactionArtifactBuildRequest request{};
    request.writer_lock = &writer;
    request.active_slot = image.selected_manifest;
    request.base_temporary_name = "base-g0000-c3502.tmp";
    request.base_relative_name = "base-g0000-c3502.vhdx";
    request.base_identity.file_identity.file_write_guid[0] =
        std::byte{0x41};
    request.base_identity.file_identity.data_write_guid[0] =
        std::byte{0x42};
    request.base_identity.virtual_disk_id =
        image.backing_identity.virtual_disk_id;
    request.journal_relative_name = "journal-e0002-c3502.rdj";
    request.journal_identity.journal_uuid[0] = std::byte{0x51};
    request.journal_epoch = image.journal_epoch + 1;
    request.operation_id = 3502;
    request.extent_options.max_extent_bytes = kVhdxDataBlockBytes;
    request.vhdx_limits = {4096, 4096, kVhdxDataBlockBytes};
    request.limits.max_live_extents = 4096;
    request.offline_vhdx_validator =
        [&offline_validation_calls](
            const OperationContext&,
            const std::filesystem::path& temporary_path) {
            ++offline_validation_calls;
            if (!std::filesystem::exists(temporary_path)) {
                return Result<void>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(
                        PersistenceErrorCode::VerificationFailed),
                    L"m3_compaction_tests",
                    L"temporary VHDX was absent during offline validation",
                    0});
            }
            return Result<void>::success();
        };
    return request;
}

class EmptyJournalLayer final : public detail::ICompositeJournalLayer {
public:
    Result<detail::CompositeJournalRead> read_block(
        const OperationContext&,
        std::uint64_t,
        std::span<std::byte>) const override {
        return Result<detail::CompositeJournalRead>::success(
            {detail::CompositeJournalKind::Absent});
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext&,
        std::uint64_t) const override {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
};

class OneDataBaseLayer final : public detail::ICompositeBaseLayer {
public:
    Result<detail::CompositeBaseRead> read_block(
        const OperationContext& context,
        const std::uint64_t block_index,
        const std::span<std::byte> destination) const override {
        const auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<detail::CompositeBaseRead>::failure(live.error());
        }
        if (block_index != 0 || destination.size() != kM2BlockSize) {
            return Result<detail::CompositeBaseRead>::success(
                {detail::CompositeBaseKind::Absent});
        }
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] = static_cast<std::byte>(
                (index * 17U + 0x39U) & 0xFFU);
        }
        return Result<detail::CompositeBaseRead>::success(
            {detail::CompositeBaseKind::Data});
    }

    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext& context,
        const std::uint64_t first_block) const override {
        const auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<std::optional<std::uint64_t>>::failure(
                live.error());
        }
        return Result<std::optional<std::uint64_t>>::success(
            first_block == 0 ? std::optional<std::uint64_t>{0}
                             : std::nullopt);
    }
};

std::unique_ptr<DynamicBlockStore> make_composite_data_store(
    const RecoveredImage& image) {
    auto composite = CompositeBackingStore::create_for_testing(
        image.backing_identity,
        std::make_unique<EmptyJournalLayer>(),
        std::make_unique<OneDataBaseLayer>());
    REQUIRE(composite.ok());
    std::shared_ptr<const IBackingStore> backing(
        composite.move_value());
    auto store = DynamicBlockStore::create({
        image.geometry.virtual_bytes,
        image.geometry.logical_sector_size,
        compaction_store_options(),
        InitialBlockState::BackingUnresident,
        std::move(backing),
        image.checkpoint_generation,
    });
    REQUIRE(store.ok());
    return store.move_value();
}

TEST_CASE(compaction_phase_contract_is_frozen_in_execution_order) {
    constexpr std::array expected{
        std::pair{CompactionPhase::OperationGateAcquired,
                  std::string_view{"operation-gate-acquired"}},
        std::pair{CompactionPhase::DurableViewCaptured,
                  std::string_view{"durable-view-captured"}},
        std::pair{CompactionPhase::VhdxCreateBefore,
                  std::string_view{"compaction.vhdx.create.before"}},
        std::pair{CompactionPhase::VhdxCreateAfter,
                  std::string_view{"compaction.vhdx.create.after"}},
        std::pair{CompactionPhase::VhdxWriteBefore,
                  std::string_view{"compaction.vhdx.write.before"}},
        std::pair{CompactionPhase::VhdxWriteAfter,
                  std::string_view{"compaction.vhdx.write.after"}},
        std::pair{CompactionPhase::VhdxFlushBefore,
                  std::string_view{"compaction.vhdx.flush.before"}},
        std::pair{CompactionPhase::VhdxFlushAfter,
                  std::string_view{"compaction.vhdx.flush.after"}},
        std::pair{CompactionPhase::VhdxReadbackBefore,
                  std::string_view{"compaction.vhdx.readback.before"}},
        std::pair{CompactionPhase::VhdxReadbackAfter,
                  std::string_view{"compaction.vhdx.readback.after"}},
        std::pair{CompactionPhase::VhdxValidateBefore,
                  std::string_view{"compaction.vhdx.validate.before"}},
        std::pair{CompactionPhase::VhdxValidateAfter,
                  std::string_view{"compaction.vhdx.validate.after"}},
        std::pair{CompactionPhase::VhdxWindowsValidateBefore,
                  std::string_view{"compaction.vhdx.windows_validate.before"}},
        std::pair{CompactionPhase::VhdxWindowsValidateAfter,
                  std::string_view{"compaction.vhdx.windows_validate.after"}},
        std::pair{CompactionPhase::VhdxRenameBefore,
                  std::string_view{"compaction.vhdx.rename.before"}},
        std::pair{CompactionPhase::VhdxRenameAfter,
                  std::string_view{"compaction.vhdx.rename.after"}},
        std::pair{CompactionPhase::JournalCreateBefore,
                  std::string_view{"compaction.journal.create.before"}},
        std::pair{CompactionPhase::JournalCreateAfter,
                  std::string_view{"compaction.journal.create.after"}},
        std::pair{CompactionPhase::JournalFlushBefore,
                  std::string_view{"compaction.journal.flush.before"}},
        std::pair{CompactionPhase::JournalFlushAfter,
                  std::string_view{"compaction.journal.flush.after"}},
        std::pair{CompactionPhase::FilesReady,
                  std::string_view{"files-ready"}},
        std::pair{CompactionPhase::BarrierValidateBefore,
                  std::string_view{"barrier-validate-before"}},
        std::pair{CompactionPhase::BarrierValidateAfter,
                  std::string_view{"barrier-validate-after"}},
        std::pair{CompactionPhase::SnapshotBeginBefore,
                  std::string_view{"snapshot-begin-before"}},
        std::pair{CompactionPhase::SnapshotBeginAfter,
                  std::string_view{"snapshot-begin-after"}},
        std::pair{CompactionPhase::DeltaWriteBefore,
                  std::string_view{"delta-write-before"}},
        std::pair{CompactionPhase::DeltaWriteAfter,
                  std::string_view{"delta-write-after"}},
        std::pair{CompactionPhase::ManifestPublishBefore,
                  std::string_view{"manifest-publish-before"}},
        std::pair{CompactionPhase::ManifestPublishAfter,
                  std::string_view{"manifest-publish-after"}},
        std::pair{CompactionPhase::BackingInstallBefore,
                  std::string_view{"backing-install-before"}},
        std::pair{CompactionPhase::BackingInstallAfter,
                  std::string_view{"backing-install-after"}},
        std::pair{CompactionPhase::ConditionalAckBefore,
                  std::string_view{"conditional-ack-before"}},
        std::pair{CompactionPhase::ConditionalAckAfter,
                  std::string_view{"conditional-ack-after"}},
        std::pair{CompactionPhase::SnapshotFinishBefore,
                  std::string_view{"snapshot-finish-before"}},
        std::pair{CompactionPhase::SnapshotFinishAfter,
                  std::string_view{"snapshot-finish-after"}},
        std::pair{CompactionPhase::Complete,
                  std::string_view{"complete"}},
    };

    REQUIRE_EQ(compaction_phase_count(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        REQUIRE_EQ(static_cast<std::size_t>(expected[index].first), index);
        REQUIRE_EQ(to_string(expected[index].first), expected[index].second);
    }
    REQUIRE_EQ(to_string(static_cast<CompactionPhase>(0xFF)),
               std::string_view{"unknown"});
}

TEST_CASE(should_compact_reports_every_enabled_reason_at_exact_boundaries) {
    CompactionPolicy policy{};
    policy.journal_physical_bytes_at_or_above = 100;
    policy.journal_to_base_ratio_at_or_above = CompactionRatio{3, 2};
    policy.checkpoints_at_or_above = 7;
    policy.index_depth_at_or_above = 4;

    const auto result = should_compact(
        CompactionMetrics{100, 200, 7, 4}, policy);

    REQUIRE(result.recommended);
    REQUIRE(result.journal_physical_bytes);
    REQUIRE(!result.journal_to_base_ratio);
    REQUIRE(result.checkpoint_count);
    REQUIRE(result.index_depth);

    const auto ratio_boundary = should_compact(
        CompactionMetrics{300, 200, 0, 0}, policy);
    REQUIRE(ratio_boundary.recommended);
    REQUIRE(ratio_boundary.journal_to_base_ratio);
}

TEST_CASE(should_compact_is_a_pure_recommendation_with_disabled_thresholds) {
    const CompactionMetrics metrics{999, 1, 999, 999};
    const CompactionPolicy disabled{};

    const auto first = should_compact(metrics, disabled);
    const auto second = should_compact(metrics, disabled);

    REQUIRE_EQ(first, second);
    REQUIRE(!first.recommended);
    REQUIRE(!first.journal_physical_bytes);
    REQUIRE(!first.journal_to_base_ratio);
    REQUIRE(!first.checkpoint_count);
    REQUIRE(!first.index_depth);
}

TEST_CASE(should_compact_handles_zero_base_and_zero_ratio_inputs) {
    CompactionPolicy policy{};
    policy.journal_to_base_ratio_at_or_above = CompactionRatio{1, 1};

    REQUIRE(should_compact(CompactionMetrics{1, 0, 0, 0}, policy)
                .journal_to_base_ratio);
    REQUIRE(!should_compact(CompactionMetrics{0, 0, 0, 0}, policy)
                 .journal_to_base_ratio);

    policy.journal_to_base_ratio_at_or_above = CompactionRatio{0, 1};
    const auto zero_numerator =
        should_compact(CompactionMetrics{1, 0, 0, 0}, policy);
    REQUIRE(!zero_numerator.policy_valid);
    REQUIRE(!zero_numerator.recommended);
    REQUIRE(!zero_numerator.journal_to_base_ratio);
    policy.journal_to_base_ratio_at_or_above = CompactionRatio{1, 0};
    const auto zero_denominator =
        should_compact(CompactionMetrics{1, 0, 0, 0}, policy);
    REQUIRE(!zero_denominator.policy_valid);
    REQUIRE(!zero_denominator.recommended);
    REQUIRE(!zero_denominator.journal_to_base_ratio);
}

TEST_CASE(should_compact_ratio_comparison_does_not_overflow) {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    CompactionPolicy policy{};
    policy.journal_to_base_ratio_at_or_above =
        CompactionRatio{maximum - 1, maximum};

    REQUIRE(should_compact(
                CompactionMetrics{maximum, maximum, 0, 0}, policy)
                .journal_to_base_ratio);

    policy.journal_to_base_ratio_at_or_above =
        CompactionRatio{maximum, maximum - 1};
    REQUIRE(!should_compact(
                 CompactionMetrics{maximum - 1, maximum, 0, 0}, policy)
                 .journal_to_base_ratio);
}

TEST_CASE(should_compact_ratio_matches_an_independent_small_fraction_oracle) {
    CompactionPolicy policy{};
    for (std::uint64_t journal = 0; journal <= 31; ++journal) {
        for (std::uint64_t base = 1; base <= 31; ++base) {
            for (std::uint64_t numerator = 1; numerator <= 31; ++numerator) {
                for (std::uint64_t denominator = 1; denominator <= 31;
                     ++denominator) {
                    policy.journal_to_base_ratio_at_or_above =
                        CompactionRatio{numerator, denominator};
                    const bool oracle =
                        journal * denominator >= numerator * base;
                    const auto actual = should_compact(
                        CompactionMetrics{journal, base, 0, 0}, policy);
                    REQUIRE(actual.policy_valid);
                    REQUIRE_EQ(actual.journal_to_base_ratio, oracle);
                    REQUIRE_EQ(actual.recommended, oracle);
                }
            }
        }
    }
}

TEST_CASE(invalid_ratio_fails_closed_even_when_another_threshold_matches) {
    CompactionPolicy policy{};
    policy.journal_physical_bytes_at_or_above = 1;
    policy.journal_to_base_ratio_at_or_above = CompactionRatio{1, 0};

    const auto result = should_compact(
        CompactionMetrics{100, 100, 0, 0}, policy);
    REQUIRE(!result.policy_valid);
    REQUIRE(result.journal_physical_bytes);
    REQUIRE(!result.recommended);
}

TEST_CASE(compaction_begin_holds_the_single_gate_but_live_mutations_continue) {
    auto session = ImageSession::ready();
    auto store = make_live_store();
    CompactionCoordinator coordinator(session, *store);
    std::vector<CompactionPhase> phases;

    auto begun = coordinator.begin_for_testing(
        bounded_context(),
        [&phases](const CompactionPhase phase) {
            phases.push_back(phase);
            return Result<void>::success();
        });
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    REQUIRE(operation.active());
    REQUIRE_EQ(operation.captured_view().generation, 7ull);
    REQUIRE(operation.captured_view().store != nullptr);
    REQUIRE_EQ(phases, (std::vector<CompactionPhase>{
        CompactionPhase::OperationGateAcquired,
        CompactionPhase::DurableViewCaptured}));

    std::array<std::byte, kM2BlockSize> data{};
    data.fill(std::byte{0x5A});
    REQUIRE(store->write(bounded_context(), 0, data).ok());

    const auto checkpoint = session.begin_checkpoint();
    REQUIRE(!checkpoint.ok());
    REQUIRE_EQ(checkpoint.error().code,
        static_cast<std::uint32_t>(PersistenceErrorCode::OperationBusy));
    const auto second_compaction = session.begin_compaction();
    REQUIRE(!second_compaction.ok());
    REQUIRE_EQ(second_compaction.error().code,
        static_cast<std::uint32_t>(PersistenceErrorCode::OperationBusy));
    REQUIRE_EQ(session.phase(), SessionPhase::Compacting);

    REQUIRE(operation.abort().ok());
    REQUIRE(!operation.active());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
}

TEST_CASE(compaction_begin_failure_releases_gate_and_frozen_view) {
    auto session = ImageSession::ready();
    auto store = make_live_store(11);
    CompactionCoordinator coordinator(session, *store);

    const auto failed = coordinator.begin_for_testing(
        bounded_context(),
        [](const CompactionPhase phase) -> Result<void> {
            if (phase == CompactionPhase::DurableViewCaptured) {
                return Result<void>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
                    L"m3_compaction_tests",
                    L"injected capture-boundary failure",
                    0});
            }
            return Result<void>::success();
        });
    REQUIRE(!failed.ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    auto checkpoint = session.begin_checkpoint();
    REQUIRE(checkpoint.ok());
    REQUIRE(checkpoint.value().abort().ok());
}

TEST_CASE(compaction_begin_rejects_cancelled_context_before_taking_gate) {
    auto session = ImageSession::ready();
    auto store = make_live_store();
    CompactionCoordinator coordinator(session, *store);
    std::stop_source stop;
    stop.request_stop();

    const auto result = coordinator.begin(OperationContext{
        stop.get_token(),
        std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled));
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
}

TEST_CASE(compaction_builds_verified_artifacts_from_durable_G_not_live_RAM) {
    CompactionSandbox sandbox;
    auto create_request = make_create_request(sandbox.root());
    auto created = create_generation_zero_image(
        bounded_context(), create_request);
    REQUIRE(created.ok());
    auto store = open_recovered_dynamic_store(
        created.value(), compaction_store_options());
    REQUIRE(store.ok());
    auto writer = acquire_bundle_writer_lock(
        sandbox.root(), create_request.manifest_relative_name);
    REQUIRE(writer.ok());
    auto session = ImageSession::ready();
    CompactionCoordinator coordinator(session, *store.value());

    std::vector<CompactionPhase> phases;
    bool live_write_succeeded = false;
    bool checkpoint_was_busy = false;
    bool second_compaction_was_busy = false;
    auto begun = coordinator.begin_for_testing(
        bounded_context(),
        [&](const CompactionPhase phase) -> Result<void> {
            phases.push_back(phase);
            if (phase == CompactionPhase::VhdxWriteBefore) {
                std::array<std::byte, kM2BlockSize> data{};
                data.fill(std::byte{0x6A});
                live_write_succeeded =
                    store.value()->write(bounded_context(), 0, data).ok();
                const auto checkpoint = session.begin_checkpoint();
                checkpoint_was_busy = !checkpoint.ok() &&
                    checkpoint.error().code == static_cast<std::uint32_t>(
                        PersistenceErrorCode::OperationBusy);
                const auto second = session.begin_compaction();
                second_compaction_was_busy = !second.ok() &&
                    second.error().code == static_cast<std::uint32_t>(
                        PersistenceErrorCode::OperationBusy);
            }
            return Result<void>::success();
        });
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t offline_validation_calls = 0;
    auto request = make_artifact_request(
        writer.value(), created.value(), offline_validation_calls);

    const auto built = operation.build_artifacts(
        bounded_context(), request);
    REQUIRE(built.ok());
    REQUIRE(operation.active());
    REQUIRE(operation.artifacts_ready());
    REQUIRE(live_write_succeeded);
    REQUIRE(checkpoint_was_busy);
    REQUIRE(second_compaction_was_busy);
    REQUIRE_EQ(offline_validation_calls, 1ull);
    REQUIRE_EQ(built.value().source_generation, 0ull);
    REQUIRE_EQ(built.value().extents.extents_emitted, 0ull);
    REQUIRE_EQ(built.value().candidate_payloads, 0ull);
    REQUIRE_EQ(built.value().journal_header_bytes, kJournalAlignment);
    REQUIRE_EQ(phases, (std::vector<CompactionPhase>{
        CompactionPhase::OperationGateAcquired,
        CompactionPhase::DurableViewCaptured,
        CompactionPhase::VhdxCreateBefore,
        CompactionPhase::VhdxCreateAfter,
        CompactionPhase::VhdxWriteBefore,
        CompactionPhase::VhdxWriteAfter,
        CompactionPhase::VhdxFlushBefore,
        CompactionPhase::VhdxFlushAfter,
        CompactionPhase::VhdxReadbackBefore,
        CompactionPhase::VhdxReadbackAfter,
        CompactionPhase::VhdxValidateBefore,
        CompactionPhase::VhdxValidateAfter,
        CompactionPhase::VhdxWindowsValidateBefore,
        CompactionPhase::VhdxWindowsValidateAfter,
        CompactionPhase::VhdxRenameBefore,
        CompactionPhase::VhdxRenameAfter,
        CompactionPhase::JournalCreateBefore,
        CompactionPhase::JournalCreateAfter,
        CompactionPhase::JournalFlushBefore,
        CompactionPhase::JournalFlushAfter,
        CompactionPhase::FilesReady}));

    REQUIRE(!std::filesystem::exists(
        sandbox.root() / request.base_temporary_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / request.base_relative_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / request.journal_relative_name));
    REQUIRE_EQ(std::filesystem::file_size(
        sandbox.root() / request.journal_relative_name),
        static_cast<std::uintmax_t>(kJournalAlignment));

    const auto* published = operation.published_base();
    REQUIRE(published != nullptr);
    auto base_capability = open_bundle_file_for_inspection(
        sandbox.root(), request.base_relative_name,
        published->windows_identity());
    REQUIRE(base_capability.ok());
    auto base_file = open_validated_persistence_file(
        base_capability.move_value());
    REQUIRE(base_file.ok());
    auto reader = VhdxReader::create(
        bounded_context(), base_file.move_value(), published->layout(),
        {{4 * kVhdxBatPageBytes, 4}, 16, 16});
    REQUIRE(reader.ok());
    std::array<std::byte, kM2BlockSize> block{};
    auto loaded = reader.value()->read_block(bounded_context(), 0, block);
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value().kind, BlockLoadKind::AllZero);

    const auto* journal_header = operation.new_journal_header();
    REQUIRE(journal_header != nullptr);
    REQUIRE_EQ(journal_header->base_checkpoint_generation, 0ull);
    REQUIRE_EQ(journal_header->epoch, 2ull);
    REQUIRE_EQ(journal_header->predecessor_kind,
        JournalPredecessorKind::Checkpoint);
    REQUIRE_EQ(journal_header->predecessor_epoch, 1ull);
    REQUIRE_EQ(journal_header->predecessor_journal_identity,
        created.value().backing_identity.journal_identity);
    REQUIRE_EQ(journal_header->predecessor_digest,
        created.value().backing_identity.commit.chain_digest);

    REQUIRE(operation.abort().ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE(writer.value().close().ok());
}

TEST_CASE(compaction_payload_bridge_preserves_nonzero_durable_extents) {
    CompactionSandbox sandbox;
    auto create_request = make_create_request(sandbox.root());
    auto created = create_generation_zero_image(
        bounded_context(), create_request);
    REQUIRE(created.ok());
    auto store = make_composite_data_store(created.value());
    auto writer = acquire_bundle_writer_lock(
        sandbox.root(), create_request.manifest_relative_name);
    REQUIRE(writer.ok());
    auto session = ImageSession::ready();
    CompactionCoordinator coordinator(session, *store);
    auto begun = coordinator.begin(bounded_context());
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t offline_validation_calls = 0;
    auto request = make_artifact_request(
        writer.value(), created.value(), offline_validation_calls);

    auto built = operation.build_artifacts(bounded_context(), request);
    REQUIRE(built.ok());
    REQUIRE_EQ(built.value().extents.extents_emitted, 1ull);
    REQUIRE_EQ(built.value().extents.data_blocks, 1ull);
    REQUIRE_EQ(built.value().candidate_payloads, 1ull);

    const auto* published = operation.published_base();
    REQUIRE(published != nullptr);
    auto capability = open_bundle_file_for_inspection(
        sandbox.root(), request.base_relative_name,
        published->windows_identity());
    REQUIRE(capability.ok());
    auto file = open_validated_persistence_file(capability.move_value());
    REQUIRE(file.ok());
    auto reader = VhdxReader::create(
        bounded_context(), file.move_value(), published->layout(),
        {{4 * kVhdxBatPageBytes, 4}, 16, 16});
    REQUIRE(reader.ok());
    std::array<std::byte, kM2BlockSize> actual{};
    auto loaded = reader.value()->read_block(
        bounded_context(), 0, actual);
    REQUIRE(loaded.ok());
    REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
    for (std::size_t index = 0; index < actual.size(); ++index) {
        REQUIRE_EQ(actual[index], static_cast<std::byte>(
            (index * 17U + 0x39U) & 0xFFU));
    }

    REQUIRE(operation.abort().ok());
    REQUIRE(writer.value().close().ok());
}

TEST_CASE(compaction_precommit_validation_failure_keeps_old_generation_authoritative) {
    CompactionSandbox sandbox;
    auto create_request = make_create_request(sandbox.root());
    auto created = create_generation_zero_image(
        bounded_context(), create_request);
    REQUIRE(created.ok());
    auto store = open_recovered_dynamic_store(
        created.value(), compaction_store_options());
    REQUIRE(store.ok());
    auto writer = acquire_bundle_writer_lock(
        sandbox.root(), create_request.manifest_relative_name);
    REQUIRE(writer.ok());
    auto session = ImageSession::ready();
    CompactionCoordinator coordinator(session, *store.value());
    auto begun = coordinator.begin(bounded_context());
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t ignored_calls = 0;
    auto request = make_artifact_request(
        writer.value(), created.value(), ignored_calls);
    request.offline_vhdx_validator = [](
        const OperationContext&,
        const std::filesystem::path&) {
        return Result<void>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(
                PersistenceErrorCode::VerificationFailed),
            L"m3_compaction_tests",
            L"injected offline validation failure",
            0});
    };

    const auto failed = operation.build_artifacts(
        bounded_context(), request);
    REQUIRE(!failed.ok());
    REQUIRE(!operation.active());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE(!std::filesystem::exists(
        sandbox.root() / request.base_relative_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / request.base_temporary_name));
    REQUIRE(!std::filesystem::exists(
        sandbox.root() / request.journal_relative_name));

    auto recovered = recover_complete_image(
        bounded_context(), sandbox.root(),
        create_request.manifest_relative_name, create_request.recovery);
    REQUIRE(recovered.ok());
    REQUIRE(recovered.image.has_value());
    REQUIRE_EQ(recovered.image->checkpoint_generation, 0ull);
    REQUIRE_EQ(recovered.image->selected_manifest,
        created.value().selected_manifest);
    REQUIRE(writer.value().close().ok());
}

}  // namespace
}  // namespace myrd::persistence
