#include "test_framework.h"

#include "src/persistence/recovery_engine.h"
#include "tests/support/m3_independent_recovery_fixture.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace myrd::persistence {
namespace {

namespace golden = test::m3_recovery_oracle;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

enum class OracleRejection {
    SlotDecode,
    BaseIdentity,
    JournalCommit,
    JournalRoot,
    JournalDigest,
};

enum class OracleBlockKind {
    Data,
    Zero,
    Discard,
    BaseAbsent,
};

struct OracleSelection final {
    std::uint8_t selected_slot{};
    std::uint64_t generation{};
    bool fallback{};
    std::map<std::uint8_t, OracleRejection> rejections{};
};

struct OracleBlock final {
    OracleBlockKind kind{OracleBlockKind::BaseAbsent};
    std::vector<std::byte> bytes{};
};

RecoveryOptions recovery_options() {
    RecoveryOptions options{};
    options.limits = {
        64 * kMiB, 64 * kMiB, 8192,
        64 * 4096, 64,
        64 * kMiB, 64,
        2 * kMiB, 2 * kMiB};
    return options;
}

std::string rejection_name(const OracleRejection rejection) {
    switch (rejection) {
    case OracleRejection::SlotDecode: return "slot_decode";
    case OracleRejection::BaseIdentity: return "base_identity";
    case OracleRejection::JournalCommit: return "journal_commit";
    case OracleRejection::JournalRoot: return "journal_root";
    case OracleRejection::JournalDigest: return "journal_digest";
    }
    return "invalid";
}

std::string slot_name(const std::uint8_t slot) {
    return slot == 0 ? "A" : "B";
}

void require_selection(const RecoveryOutcome& outcome,
                       const OracleSelection& oracle) {
    REQUIRE(outcome.ok());
    REQUIRE(outcome.image.has_value());
    REQUIRE_EQ(outcome.image->selected_slot, oracle.selected_slot);
    REQUIRE_EQ(outcome.image->checkpoint_generation, oracle.generation);
    REQUIRE_EQ(outcome.image->fallback, oracle.fallback);
    REQUIRE_EQ(outcome.image->backing_view.generation, oracle.generation);
    REQUIRE(outcome.image->backing_view.store != nullptr);
    for (std::uint8_t slot = 0; slot < 2; ++slot) {
        REQUIRE_EQ(outcome.diagnostics[slot].selected,
                   slot == oracle.selected_slot);
        const auto rejected = oracle.rejections.find(slot);
        const auto expected = rejected == oracle.rejections.end()
            ? "slot=" + slot_name(slot) + ";check=none;status=passed"
            : "slot=" + slot_name(slot) + ";check=" +
                  rejection_name(rejected->second) + ";status=corrupt";
        REQUIRE_EQ(outcome.diagnostics[slot].safe_context, expected);
    }
}

void require_rejection(const RecoveryOutcome& outcome,
                       const std::uint8_t slot,
                       const OracleRejection rejection) {
    REQUIRE(!outcome.ok());
    REQUIRE(!outcome.image.has_value());
    const auto expected = "slot=" + slot_name(slot) + ";check=" +
        rejection_name(rejection) + ";status=corrupt";
    REQUIRE_EQ(outcome.diagnostics[slot].safe_context, expected);
}

RecoveryOutcome recover(const golden::IndependentRecoveryFixture& fixture) {
    return recover_complete_image(
        OperationContext{}, fixture.root(), "disk.rdm", recovery_options());
}

std::map<std::uint64_t, OracleBlock> block_oracle() {
    std::vector<std::byte> data(golden::kLogicalBlockBytes);
    data[0] = std::byte{0xaa};
    data[32768] = std::byte{0xbb};
    data[65535] = std::byte{0xcc};
    return {
        {3, {OracleBlockKind::Data, std::move(data)}},
        {4, {OracleBlockKind::Zero, {}}},
        {5, {OracleBlockKind::Discard, {}}},
    };
}

void require_all_blocks(const RecoveryOutcome& outcome) {
    REQUIRE(outcome.ok());
    const auto oracle = block_oracle();
    for (std::uint64_t block = 0; block < golden::kLogicalBlockCount;
         ++block) {
        std::array<std::byte, golden::kLogicalBlockBytes> destination{};
        destination.fill(std::byte{0xa5});
        const auto loaded = outcome.image->backing_view.store->load_block(
            OperationContext{}, block, destination);
        REQUIRE(loaded.ok());
        const auto found = oracle.find(block);
        const OracleBlock expected = found == oracle.end()
            ? OracleBlock{OracleBlockKind::BaseAbsent, {}}
            : found->second;
        if (expected.kind == OracleBlockKind::Data) {
            REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
            REQUIRE(std::equal(destination.begin(), destination.end(),
                               expected.bytes.begin(), expected.bytes.end()));
        } else {
            REQUIRE_EQ(loaded.value().kind, BlockLoadKind::AllZero);
            REQUIRE(std::all_of(
                destination.begin(), destination.end(),
                [](const std::byte value) {
                    return value == std::byte{0xa5};
                }));
        }
    }
}

TEST_CASE(recovery_oracle_selects_a_only_b_only_equivalent_and_fallback) {
    {
        golden::IndependentRecoveryFixture fixture;
        fixture.publish(golden::SlotSpec{0}, std::nullopt);
        const auto outcome = recover(fixture);
        require_selection(
            outcome, {0, 5, false,
                      {{std::uint8_t{1}, OracleRejection::SlotDecode}}});
        require_all_blocks(outcome);
    }
    {
        golden::IndependentRecoveryFixture fixture;
        fixture.publish(std::nullopt, golden::SlotSpec{1});
        const auto outcome = recover(fixture);
        require_selection(
            outcome, {1, 5, false,
                      {{std::uint8_t{0}, OracleRejection::SlotDecode}}});
    }
    {
        golden::IndependentRecoveryFixture fixture;
        fixture.publish(golden::SlotSpec{0}, golden::SlotSpec{1});
        const auto outcome = recover(fixture);
        require_selection(outcome, {0, 5, false, {}});
    }
    {
        golden::IndependentRecoveryFixture fixture;
        fixture.publish(
            golden::SlotSpec{0},
            golden::SlotSpec{1, 6, golden::SlotMutation::BaseIdentity});
        const auto outcome = recover(fixture);
        require_selection(
            outcome, {0, 5, true,
                      {{std::uint8_t{1}, OracleRejection::BaseIdentity}}});
    }
}

TEST_CASE(recovery_oracle_rejects_manifest_identity_root_and_digest_mutations) {
    const auto verify = [](const golden::SlotMutation mutation,
                           const OracleRejection rejection) {
        golden::IndependentRecoveryFixture fixture;
        fixture.publish(golden::SlotSpec{0, 5, mutation}, std::nullopt);
        require_rejection(recover(fixture), 0, rejection);
    };
    verify(golden::SlotMutation::BaseIdentity,
           OracleRejection::BaseIdentity);
    verify(golden::SlotMutation::RootCrc,
           OracleRejection::JournalCommit);
    verify(golden::SlotMutation::ChainDigest,
           OracleRejection::JournalCommit);
}

TEST_CASE(recovery_oracle_locates_root_crc_and_chain_coverage_corruption) {
    {
        golden::IndependentRecoveryFixture fixture;
        fixture.publish(golden::SlotSpec{0}, std::nullopt);
        fixture.damage_root_record();
        require_rejection(recover(fixture), 0,
                          OracleRejection::JournalRoot);
    }
    {
        golden::IndependentRecoveryFixture fixture;
        fixture.publish(golden::SlotSpec{0}, std::nullopt);
        fixture.damage_chain_covered_data();
        require_rejection(recover(fixture), 0,
                          OracleRejection::JournalDigest);
    }
}

}  // namespace
}  // namespace myrd::persistence
