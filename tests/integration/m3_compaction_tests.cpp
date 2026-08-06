#include "test_framework.h"

#include "src/persistence/compaction_coordinator.h"
#include "tests/support/fake_backing_store.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
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
        test::FakeBackingStoreBuilder(91).build(),
        generation,
    });
    REQUIRE(created.ok());
    return created.move_value();
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

}  // namespace
}  // namespace myrd::persistence
