#include "test_framework.h"

#include "src/common/sha256_stream.h"
#include "src/persistence/compaction_coordinator.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/recovered_store_adapter.h"
#include "src/persistence/vhdx_reader.h"
#include "tests/support/fake_backing_store.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <virtdisk.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
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

std::vector<CompactionPhase> all_compaction_phases() {
    std::vector<CompactionPhase> phases;
    phases.reserve(compaction_phase_count());
    for (std::size_t index = 0; index < compaction_phase_count(); ++index) {
        phases.push_back(static_cast<CompactionPhase>(index));
    }
    return phases;
}

std::vector<std::byte> read_all_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    const std::vector<char> chars{
        std::istreambuf_iterator<char>{input},
        std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(chars.size());
    for (std::size_t index = 0; index < chars.size(); ++index) {
        bytes[index] = static_cast<std::byte>(
            static_cast<unsigned char>(chars[index]));
    }
    return bytes;
}

enum class OracleBlockKind : std::uint8_t {
    CleanZero,
    Data,
    Zero,
    Discard,
};

struct OracleBlock final {
    OracleBlockKind kind{OracleBlockKind::CleanZero};
    std::uint32_t seed{};
};

void fill_oracle_data(
    const std::uint64_t block_index,
    const std::uint32_t seed,
    const std::span<std::byte> destination) {
    for (std::size_t index = 0; index < destination.size(); ++index) {
        destination[index] = static_cast<std::byte>(
            (block_index * 131U + index * 17U + seed) & 0xFFU);
    }
}

Result<WindowsFileIdentity> validate_with_open_virtual_disk(
    const OperationContext& context,
    const std::filesystem::path& path) {
    auto live = check_operation_context(context);
    if (!live.ok()) {
        return Result<WindowsFileIdentity>::failure(live.error());
    }
    struct ModuleGuard final {
        HMODULE module{};
        ~ModuleGuard() noexcept {
            if (module != nullptr) FreeLibrary(module);
        }
    } module{LoadLibraryW(L"virtdisk.dll")};
    if (module.module == nullptr) {
        return Result<WindowsFileIdentity>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"validate_with_open_virtual_disk",
            L"virtdisk.dll is unavailable",
            GetLastError()});
    }
    using OpenVirtualDiskFn = decltype(&OpenVirtualDisk);
    const auto open_virtual_disk = reinterpret_cast<OpenVirtualDiskFn>(
        GetProcAddress(module.module, "OpenVirtualDisk"));
    if (open_virtual_disk == nullptr) {
        return Result<WindowsFileIdentity>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(PersistenceErrorCode::IoError),
            L"validate_with_open_virtual_disk",
            L"OpenVirtualDisk is unavailable",
            GetLastError()});
    }
    constexpr GUID microsoft_vendor{
        0xec984aec, 0xa0f9, 0x47e9,
        {0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b}};
    VIRTUAL_STORAGE_TYPE storage{
        VIRTUAL_STORAGE_TYPE_DEVICE_VHDX, microsoft_vendor};
    OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    parameters.Version1.RWDepth = 1;
    HANDLE raw_disk{};
    const auto status = open_virtual_disk(
        &storage, path.c_str(), VIRTUAL_DISK_ACCESS_GET_INFO,
        OPEN_VIRTUAL_DISK_FLAG_NONE, &parameters, &raw_disk);
    if (status != ERROR_SUCCESS) {
        return Result<WindowsFileIdentity>::failure({
            ErrorDomain::persistence,
            static_cast<std::uint32_t>(
                PersistenceErrorCode::VerificationFailed),
            L"validate_with_open_virtual_disk",
            L"Windows rejected the pinned VHDX",
            status});
    }
    UniqueHandle disk(raw_disk);
    auto observed = open_bundle_file_for_inspection(
        path.parent_path(), path.filename().string());
    if (!observed.ok()) {
        return Result<WindowsFileIdentity>::failure(observed.error());
    }
    return Result<WindowsFileIdentity>::success(
        observed.value().identity());
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
        [&offline_validation_calls, &writer](
            const OperationContext&,
            const std::filesystem::path& temporary_path)
            -> Result<WindowsFileIdentity> {
            ++offline_validation_calls;
            if (!std::filesystem::exists(temporary_path)) {
                return Result<WindowsFileIdentity>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(
                        PersistenceErrorCode::VerificationFailed),
                    L"m3_compaction_tests",
                    L"temporary VHDX was absent during offline validation",
                    0});
            }
            auto forbidden_write_reopen = open_bundle_file_for_mutation(
                writer, temporary_path.filename().string());
            if (forbidden_write_reopen.ok()) {
                return Result<WindowsFileIdentity>::failure({
                    ErrorDomain::persistence,
                    static_cast<std::uint32_t>(
                        PersistenceErrorCode::GenerationConflict),
                    L"m3_compaction_tests",
                    L"validated VHDX was not pinned against writes",
                    0});
            }
            auto observed = open_bundle_file_for_inspection(
                temporary_path.parent_path(),
                temporary_path.filename().string());
            if (!observed.ok()) {
                return Result<WindowsFileIdentity>::failure(
                    observed.error());
            }
            return Result<WindowsFileIdentity>::success(
                observed.value().identity());
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

struct SparseBaseCounters final {
    std::atomic<std::uint64_t> reads{};
    std::atomic<std::uint64_t> candidate_queries{};
};

class SparseDataBaseLayer final : public detail::ICompositeBaseLayer {
public:
    SparseDataBaseLayer(
        std::vector<std::uint64_t> blocks,
        std::shared_ptr<SparseBaseCounters> counters,
        std::optional<std::uint64_t> fail_block = std::nullopt) noexcept
        : blocks_(std::move(blocks)),
          counters_(std::move(counters)),
          fail_block_(fail_block) {
        std::sort(blocks_.begin(), blocks_.end());
    }

    Result<detail::CompositeBaseRead> read_block(
        const OperationContext& context,
        const std::uint64_t block_index,
        const std::span<std::byte> destination) const override {
        const auto live = check_operation_context(context);
        if (!live.ok()) {
            return Result<detail::CompositeBaseRead>::failure(live.error());
        }
        counters_->reads.fetch_add(1, std::memory_order_relaxed);
        if (fail_block_.has_value() && block_index == *fail_block_) {
            return Result<detail::CompositeBaseRead>::failure({
                ErrorDomain::persistence,
                static_cast<std::uint32_t>(
                    PersistenceErrorCode::BackingDataCorrupt),
                L"SparseDataBaseLayer::read_block",
                L"injected immutable source corruption",
                0});
        }
        if (!std::binary_search(
                blocks_.begin(), blocks_.end(), block_index) ||
            destination.size() != kM2BlockSize) {
            return Result<detail::CompositeBaseRead>::success(
                {detail::CompositeBaseKind::Absent});
        }
        for (std::size_t index = 0; index < destination.size(); ++index) {
            destination[index] = static_cast<std::byte>(
                (block_index * 131U + index * 17U + 0x39U) & 0xFFU);
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
        counters_->candidate_queries.fetch_add(
            1, std::memory_order_relaxed);
        const auto found = std::lower_bound(
            blocks_.begin(), blocks_.end(), first_block);
        return Result<std::optional<std::uint64_t>>::success(
            found == blocks_.end()
                ? std::nullopt
                : std::optional<std::uint64_t>{*found});
    }

private:
    std::vector<std::uint64_t> blocks_;
    std::shared_ptr<SparseBaseCounters> counters_;
    std::optional<std::uint64_t> fail_block_;
};

std::unique_ptr<DynamicBlockStore> make_composite_data_store(
    const RecoveredImage& image,
    std::vector<std::uint64_t> blocks = {0},
    std::shared_ptr<SparseBaseCounters> counters =
        std::make_shared<SparseBaseCounters>(),
    std::optional<std::uint64_t> fail_block = std::nullopt) {
    auto composite = CompositeBackingStore::create_for_testing(
        image.backing_identity,
        std::make_unique<EmptyJournalLayer>(),
        std::make_unique<SparseDataBaseLayer>(
            std::move(blocks), std::move(counters), fail_block));
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

std::unique_ptr<JournalWriter> make_compaction_journal_writer() {
    JournalWriterOptions options{};
    options.index_builder = {
        {64 * kIndexNodeBytes, 64}, 4096, 4096 * kIndexNodeBytes};
    options.max_checkpoint_records = 8192;
    options.max_checkpoint_bytes = 64ull * 1024 * 1024;
    auto writer = JournalWriter::create(std::move(options));
    REQUIRE(writer.ok());
    return writer.move_value();
}

CompactionDeltaPrepareRequest make_delta_request(
    const RecoveredImage& image,
    const std::uint64_t operation_id = 3502) {
    PersistenceErrorContext error_context{};
    error_context.operation = "compaction.prepare";
    error_context.code = PersistenceErrorCode::InvalidFormat;
    error_context.phase = SessionPhase::Compacting;
    error_context.operation_id = operation_id;
    error_context.image_uuid = image.image_uuid;
    error_context.generation = image.checkpoint_generation + 1;
    error_context.disposition = CommitDisposition::NotCommitted;
    auto complete = CompletePersistenceErrorContext::create(
        std::move(error_context));
    REQUIRE(complete.has_value());
    CompactionDeltaPrepareRequest request{std::move(*complete)};
    request.volume_barrier =
        VolumeBarrierKind::RawBlockTestNoFilesystemCache;
    request.snapshot_batch_bytes = kM2BlockSize;
    return request;
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

TEST_CASE(compaction_payload_bridge_handles_shared_boundary_and_partial_payloads) {
    CompactionSandbox sandbox;
    auto create_request = make_create_request(sandbox.root());
    create_request.geometry.virtual_bytes += kM2BlockSize;
    auto created = create_generation_zero_image(
        bounded_context(), create_request);
    REQUIRE(created.ok());
    const auto block_count =
        created.value().geometry.virtual_bytes / kM2BlockSize;
    REQUIRE_EQ(block_count, 129ull);
    const std::vector<std::uint64_t> data_blocks{30, 31, 32, 33, 128};
    auto counters = std::make_shared<SparseBaseCounters>();
    auto store = make_composite_data_store(
        created.value(), data_blocks, counters);
    auto writer = acquire_bundle_writer_lock(
        sandbox.root(), create_request.manifest_relative_name);
    REQUIRE(writer.ok());
    auto session = ImageSession::ready();
    CompactionCoordinator coordinator(session, *store);
    auto begun = coordinator.begin(bounded_context());
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t offline_calls = 0;
    auto request = make_artifact_request(
        writer.value(), created.value(), offline_calls);

    const auto built = operation.build_artifacts(
        bounded_context(), request);
    REQUIRE(built.ok());
    REQUIRE_EQ(built.value().extents.data_blocks, data_blocks.size());
    REQUIRE_EQ(built.value().extents.extents_emitted, 2ull);
    REQUIRE_EQ(built.value().candidate_payloads, 3ull);
    REQUIRE(counters->candidate_queries.load(std::memory_order_relaxed) <=
        data_blocks.size() + 2);

    const auto* published = operation.published_base();
    REQUIRE(published != nullptr);
    REQUIRE_EQ(published->layout().virtual_size(),
        create_request.geometry.virtual_bytes);
    auto capability = open_bundle_file_for_inspection(
        sandbox.root(), request.base_relative_name,
        published->windows_identity());
    REQUIRE(capability.ok());
    auto file = open_validated_persistence_file(capability.move_value());
    REQUIRE(file.ok());
    auto reader = VhdxReader::create(
        bounded_context(), file.move_value(), published->layout(),
        {{8 * kVhdxBatPageBytes, 8}, 32, 32});
    REQUIRE(reader.ok());
    std::array<std::byte, kM2BlockSize> actual{};
    for (const auto block : data_blocks) {
        auto loaded = reader.value()->read_block(
            bounded_context(), block, actual);
        REQUIRE(loaded.ok());
        REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
        for (std::size_t index = 0; index < actual.size(); ++index) {
            REQUIRE_EQ(actual[index], static_cast<std::byte>(
                (block * 131U + index * 17U + 0x39U) & 0xFFU));
        }
    }
    for (const auto block : std::array<std::uint64_t, 2>{29, 34}) {
        std::fill(actual.begin(), actual.end(), std::byte{0xCC});
        auto loaded = reader.value()->read_block(
            bounded_context(), block, actual);
        REQUIRE(loaded.ok());
        REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
        REQUIRE(std::ranges::all_of(actual, [](const std::byte value) {
            return value == std::byte{0};
        }));
    }
    std::fill(actual.begin(), actual.end(), std::byte{0xCC});
    auto absent_payload = reader.value()->read_block(
        bounded_context(), 127, actual);
    REQUIRE(absent_payload.ok());
    REQUIRE_EQ(absent_payload.value().kind, BlockLoadKind::AllZero);

    REQUIRE(operation.abort().ok());
    REQUIRE(writer.value().close().ok());
}

TEST_CASE(compaction_payload_bridge_fails_closed_on_limits_and_source_corruption) {
    for (const bool corrupt_source : {false, true}) {
        CompactionSandbox sandbox;
        auto create_request = make_create_request(sandbox.root());
        auto created = create_generation_zero_image(
            bounded_context(), create_request);
        REQUIRE(created.ok());
        auto counters = std::make_shared<SparseBaseCounters>();
        auto store = make_composite_data_store(
            created.value(), {0, 64}, counters,
            corrupt_source
                ? std::optional<std::uint64_t>{64}
                : std::nullopt);
        auto writer = acquire_bundle_writer_lock(
            sandbox.root(), create_request.manifest_relative_name);
        REQUIRE(writer.ok());
        const auto manifest_before = read_all_bytes(
            sandbox.root() / create_request.manifest_relative_name);
        auto session = ImageSession::ready();
        CompactionCoordinator coordinator(session, *store);
        auto begun = coordinator.begin(bounded_context());
        REQUIRE(begun.ok());
        auto operation = begun.move_value();
        std::uint64_t offline_calls = 0;
        auto request = make_artifact_request(
            writer.value(), created.value(), offline_calls);
        if (!corrupt_source) request.limits.max_live_extents = 1;

        const auto built = operation.build_artifacts(
            bounded_context(), request);
        REQUIRE(!built.ok());
        REQUIRE_EQ(built.error().code,
            static_cast<std::uint32_t>(
                corrupt_source
                    ? PersistenceErrorCode::BackingDataCorrupt
                    : PersistenceErrorCode::SnapshotResourceExhausted));
        REQUIRE(!operation.active());
        REQUIRE_EQ(session.phase(), SessionPhase::Ready);
        REQUIRE_EQ(read_all_bytes(
            sandbox.root() / create_request.manifest_relative_name),
            manifest_before);
        auto recovered = recover_complete_image(
            bounded_context(), sandbox.root(),
            create_request.manifest_relative_name,
            create_request.recovery);
        REQUIRE(recovered.ok());
        REQUIRE(recovered.image.has_value());
        REQUIRE_EQ(recovered.image->checkpoint_generation, 0ull);
        REQUIRE(writer.value().close().ok());
    }
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
        const std::filesystem::path& temporary_path) {
        auto observed = open_bundle_file_for_inspection(
            temporary_path.parent_path(),
            temporary_path.filename().string());
        REQUIRE(observed.ok());
        auto wrong_object = observed.value().identity();
        wrong_object.file_id[0] ^= std::byte{0xFF};
        return Result<WindowsFileIdentity>::success(wrong_object);
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

TEST_CASE(compaction_pinned_share_mode_accepts_real_open_virtual_disk_validation) {
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
    std::uint64_t unused_calls = 0;
    auto request = make_artifact_request(
        writer.value(), created.value(), unused_calls);
    request.offline_vhdx_validator = validate_with_open_virtual_disk;

    const auto built = operation.build_artifacts(
        bounded_context(), request);
    REQUIRE(built.ok());
    REQUIRE(operation.artifacts_ready());
    REQUIRE(operation.abort().ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE(writer.value().close().ok());
}

TEST_CASE(compaction_files_ready_observes_late_cancellation_and_releases_gate) {
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
    std::stop_source stop;
    auto begun = coordinator.begin_for_testing(
        bounded_context(),
        [&stop](const CompactionPhase phase) {
            if (phase == CompactionPhase::FilesReady) {
                stop.request_stop();
            }
            return Result<void>::success();
        });
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t offline_calls = 0;
    auto request = make_artifact_request(
        writer.value(), created.value(), offline_calls);
    const OperationContext cancelling_context{
        stop.get_token(),
        std::chrono::steady_clock::now() + std::chrono::seconds(5)};

    const auto failed = operation.build_artifacts(
        cancelling_context, request);
    REQUIRE(!failed.ok());
    REQUIRE_EQ(failed.error().code,
        static_cast<std::uint32_t>(OperationErrorCode::cancelled));
    REQUIRE(!operation.active());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE(std::filesystem::exists(
        sandbox.root() / request.base_relative_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / request.journal_relative_name));
    REQUIRE(writer.value().close().ok());
}

TEST_CASE(compaction_cut_streams_dirty_delta_through_the_existing_checkpoint_path) {
    CompactionSandbox sandbox;
    auto create_request = make_create_request(sandbox.root());
    auto created = create_generation_zero_image(
        bounded_context(), create_request);
    REQUIRE(created.ok());
    auto store = open_recovered_dynamic_store(
        created.value(), compaction_store_options());
    REQUIRE(store.ok());
    std::array<std::byte, kM2BlockSize> data{};
    data.fill(std::byte{0x7B});
    REQUIRE(store.value()->write(bounded_context(), 0, data).ok());
    REQUIRE(store.value()->write(
        bounded_context(), kM2BlockSize, data).ok());
    REQUIRE(store.value()->write(
        bounded_context(), 2 * kM2BlockSize, data).ok());
    const DataRange zero_range{kM2BlockSize, kM2BlockSize};
    REQUIRE(store.value()->zero(
        bounded_context(), {&zero_range, 1}).ok());
    const DataRange discard_range{2 * kM2BlockSize, kM2BlockSize};
    REQUIRE(store.value()->unmap(
        bounded_context(), {&discard_range, 1}).ok());

    auto writer_lock = acquire_bundle_writer_lock(
        sandbox.root(), create_request.manifest_relative_name);
    REQUIRE(writer_lock.ok());
    auto session = ImageSession::ready();
    CompactionCoordinator coordinator(session, *store.value());
    std::vector<CompactionPhase> phases;
    auto begun = coordinator.begin_for_testing(
        bounded_context(),
        [&phases](const CompactionPhase phase) {
            phases.push_back(phase);
            return Result<void>::success();
        });
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t offline_calls = 0;
    auto artifacts = make_artifact_request(
        writer_lock.value(), created.value(), offline_calls);
    REQUIRE(operation.build_artifacts(
        bounded_context(), artifacts).ok());
    auto journal_writer = make_compaction_journal_writer();
    auto delta_request = make_delta_request(created.value());

    auto prepared = coordinator.prepare_delta(
        bounded_context(), std::move(operation), delta_request,
        *journal_writer);
    REQUIRE(prepared.ok());
    REQUIRE(!operation.active());
    REQUIRE(prepared.transaction() != nullptr);
    REQUIRE_EQ(prepared.transaction()->checkpoint().generation, 1ull);
    REQUIRE_EQ(prepared.transaction()->checkpoint().data_count, 1ull);
    REQUIRE_EQ(prepared.transaction()->checkpoint().zero_count, 1ull);
    REQUIRE_EQ(prepared.transaction()->checkpoint().discard_count, 1ull);
    REQUIRE_EQ(prepared.transaction()->checkpoint().index_node_count, 1ull);
    REQUIRE(prepared.transaction()->checkpoint().committed_length >
        kJournalAlignment);
    REQUIRE_EQ(session.phase(), SessionPhase::Compacting);
    REQUIRE_EQ(phases[phases.size() - 6],
        CompactionPhase::BarrierValidateBefore);
    REQUIRE_EQ(phases[phases.size() - 5],
        CompactionPhase::BarrierValidateAfter);
    REQUIRE_EQ(phases[phases.size() - 4],
        CompactionPhase::SnapshotBeginBefore);
    REQUIRE_EQ(phases[phases.size() - 3],
        CompactionPhase::SnapshotBeginAfter);
    REQUIRE_EQ(phases[phases.size() - 2],
        CompactionPhase::DeltaWriteBefore);
    REQUIRE_EQ(phases[phases.size() - 1],
        CompactionPhase::DeltaWriteAfter);

    auto transaction = prepared.move_transaction();
    REQUIRE(transaction.abort().ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE(writer_lock.value().close().ok());
}

TEST_CASE(compaction_empty_cut_still_writes_index_and_G_plus_one_commit) {
    CompactionSandbox sandbox;
    auto create_request = make_create_request(sandbox.root());
    auto created = create_generation_zero_image(
        bounded_context(), create_request);
    REQUIRE(created.ok());
    auto store = open_recovered_dynamic_store(
        created.value(), compaction_store_options());
    REQUIRE(store.ok());
    auto writer_lock = acquire_bundle_writer_lock(
        sandbox.root(), create_request.manifest_relative_name);
    REQUIRE(writer_lock.ok());
    auto session = ImageSession::ready();
    CompactionCoordinator coordinator(session, *store.value());
    auto begun = coordinator.begin(bounded_context());
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t offline_calls = 0;
    auto artifacts = make_artifact_request(
        writer_lock.value(), created.value(), offline_calls);
    REQUIRE(operation.build_artifacts(
        bounded_context(), artifacts).ok());
    auto journal_writer = make_compaction_journal_writer();
    auto prepared = coordinator.prepare_delta(
        bounded_context(), std::move(operation),
        make_delta_request(created.value()), *journal_writer);

    REQUIRE(prepared.ok());
    const auto& checkpoint = prepared.transaction()->checkpoint();
    REQUIRE_EQ(checkpoint.generation, 1ull);
    REQUIRE_EQ(checkpoint.data_count, 0ull);
    REQUIRE_EQ(checkpoint.zero_count, 0ull);
    REQUIRE_EQ(checkpoint.discard_count, 0ull);
    // The frozen journal format forbids zero-entry physical index nodes.
    // An all-zero root descriptor is the canonical empty COW index; the
    // G+1 commit durably publishes that empty index state.
    REQUIRE_EQ(checkpoint.index_node_count, 0ull);
    REQUIRE_EQ(checkpoint.root, PersistentIndexRootDescriptor{});
    REQUIRE_EQ(checkpoint.commit.offset, kJournalAlignment);
    REQUIRE_EQ(checkpoint.committed_length, 2 * kJournalAlignment);
    auto transaction = prepared.move_transaction();
    REQUIRE(transaction.abort().ok());
    REQUIRE(writer_lock.value().close().ok());
}

TEST_CASE(compaction_publishes_reopens_installs_and_preserves_post_cut_write) {
    CompactionSandbox sandbox;
    auto create_request = make_create_request(sandbox.root());
    auto created = create_generation_zero_image(
        bounded_context(), create_request);
    REQUIRE(created.ok());
    auto store = open_recovered_dynamic_store(
        created.value(), compaction_store_options());
    REQUIRE(store.ok());
    std::vector<std::byte> cut_data(kM2BlockSize, std::byte{0x72});
    REQUIRE(store.value()->write(
        bounded_context(), 0, cut_data).ok());

    auto writer_lock = acquire_bundle_writer_lock(
        sandbox.root(), create_request.manifest_relative_name);
    REQUIRE(writer_lock.ok());
    auto session = ImageSession::ready();
    CompactionCoordinator coordinator(session, *store.value());
    std::vector<CompactionPhase> phases;
    auto begun = coordinator.begin_for_testing(
        bounded_context(),
        [&phases](const CompactionPhase phase) {
            phases.push_back(phase);
            return Result<void>::success();
        });
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t offline_calls = 0;
    auto artifacts = make_artifact_request(
        writer_lock.value(), created.value(), offline_calls);
    REQUIRE(operation.build_artifacts(
        bounded_context(), artifacts).ok());
    auto observed_base = open_bundle_file_for_inspection(
        sandbox.root(), artifacts.base_relative_name);
    REQUIRE(observed_base.ok());
    const auto expected_base_windows_identity =
        observed_base.value().identity();
    auto journal_writer = make_compaction_journal_writer();
    auto prepared = coordinator.prepare_delta(
        bounded_context(), std::move(operation),
        make_delta_request(created.value()), *journal_writer);
    REQUIRE(prepared.ok());
    auto transaction = prepared.move_transaction();
    const auto write_reopen_while_anchored =
        open_bundle_file_for_mutation(
            writer_lock.value(), artifacts.base_relative_name);
    REQUIRE(!write_reopen_while_anchored.ok());

    std::vector<std::byte> post_cut_data(kM2BlockSize, std::byte{0x83});
    REQUIRE(store.value()->write(
        bounded_context(), 0, post_cut_data).ok());

    auto manifest_capability = open_locked_manifest_for_mutation(
        writer_lock.value());
    REQUIRE(manifest_capability.ok());
    auto manifest_file = open_validated_persistence_file(
        manifest_capability.move_value());
    REQUIRE(manifest_file.ok());
    CheckpointManifestPublishRequest publish{};
    publish.manifest_file = manifest_file.move_value();
    publish.active_slot = created.value().selected_manifest;
    publish.committed_time_100ns = 223456;
    publish.complete_validation = CompleteCandidateValidation{
        sandbox.root(), create_request.manifest_relative_name,
        create_request.recovery};

    const auto published = transaction.publish_manifest(
        bounded_context(), std::move(publish));
    REQUIRE(published.ok());
    REQUIRE_EQ(published.disposition, CommitDisposition::Committed);
    REQUIRE_EQ(published.generation, 1ull);

    const auto reconciled = transaction.reconcile(bounded_context());
    REQUIRE(reconciled.ok());
    REQUIRE(reconciled.backing_installed);
    REQUIRE(reconciled.snapshot_finished);
    REQUIRE_EQ(reconciled.ack.newer_dirty_preserved, 1ull);
    REQUIRE_EQ(store.value()->backing_generation(), 1ull);
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    REQUIRE_EQ(store.value()->stats().resident_dirty_blocks, 1ull);
    REQUIRE_EQ(phases, all_compaction_phases());
    // The temporary construction anchor is gone, but the installed runtime
    // BackingView now owns the stronger invariant: a namespace-mutating
    // handle (DELETE access) remains denied until the final view releases.
    auto namespace_mutation_while_live = open_bundle_file_for_mutation(
        writer_lock.value(), artifacts.base_relative_name,
        expected_base_windows_identity);
    REQUIRE(!namespace_mutation_while_live.ok());
    REQUIRE_EQ(namespace_mutation_while_live.error().native_code,
               static_cast<std::uint32_t>(ERROR_SHARING_VIOLATION));

    std::vector<std::byte> live_bytes(kM2BlockSize);
    REQUIRE(store.value()->read(
        bounded_context(), 0, live_bytes).ok());
    REQUIRE_EQ(live_bytes, post_cut_data);

    auto reopened = recover_complete_image(
        bounded_context(), sandbox.root(),
        create_request.manifest_relative_name, create_request.recovery);
    REQUIRE(reopened.ok());
    REQUIRE(reopened.image.has_value());
    REQUIRE_EQ(reopened.image->checkpoint_generation, 1ull);
    REQUIRE_EQ(reopened.image->base_generation, 0ull);
    REQUIRE_EQ(reopened.image->journal_epoch, 2ull);
    REQUIRE(same_file_object(
        reopened.image->backing_identity.base_windows_identity,
        expected_base_windows_identity));
    REQUIRE_EQ(reopened.image->backing_identity.base_windows_identity.size,
        expected_base_windows_identity.size);
    REQUIRE_EQ(reopened.image->selected_manifest.base_relative_name,
        artifacts.base_relative_name);
    REQUIRE_EQ(reopened.image->selected_manifest.journal_relative_name,
        artifacts.journal_relative_name);
    std::vector<std::byte> durable_bytes(kM2BlockSize);
    auto durable = reopened.image->backing_view.store->load_block(
        bounded_context(), 0, durable_bytes);
    REQUIRE(durable.ok());
    REQUIRE_EQ(durable.value().kind, BlockLoadKind::Data);
    REQUIRE_EQ(durable_bytes, cut_data);

    REQUIRE(std::filesystem::exists(
        sandbox.root() / create_request.base_relative_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / create_request.journal_relative_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / artifacts.base_relative_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / artifacts.journal_relative_name));
    REQUIRE(writer_lock.value().close().ok());
}

TEST_CASE(compaction_reopen_matches_an_independent_full_disk_cut_oracle) {
    CompactionSandbox sandbox;
    auto create_request = make_create_request(sandbox.root());
    auto created = create_generation_zero_image(
        bounded_context(), create_request);
    REQUIRE(created.ok());
    auto oracle_store_options = compaction_store_options();
    oracle_store_options.soft_limit_bytes = 32 * kM2BlockSize;
    oracle_store_options.hard_limit_bytes = 64 * kM2BlockSize;
    oracle_store_options.eviction_low_watermark_bytes =
        16 * kM2BlockSize;
    auto store = open_recovered_dynamic_store(
        created.value(), oracle_store_options);
    REQUIRE(store.ok());
    REQUIRE_EQ(created.value().geometry.virtual_bytes % kM2BlockSize, 0ull);
    const auto block_count =
        created.value().geometry.virtual_bytes / kM2BlockSize;
    REQUIRE(block_count > 40);
    std::vector<OracleBlock> oracle(
        static_cast<std::size_t>(block_count));

    const auto write_data = [&](const std::uint64_t block,
                                const std::uint32_t seed) {
        std::vector<std::byte> bytes(kM2BlockSize);
        fill_oracle_data(block, seed, bytes);
        REQUIRE(store.value()->write(
            bounded_context(), block * kM2BlockSize, bytes).ok());
        oracle[static_cast<std::size_t>(block)] =
            {OracleBlockKind::Data, seed};
    };
    const auto mark_zero = [&](const std::uint64_t block) {
        const DataRange range{block * kM2BlockSize, kM2BlockSize};
        REQUIRE(store.value()->zero(
            bounded_context(), {&range, 1}).ok());
        oracle[static_cast<std::size_t>(block)] =
            {OracleBlockKind::Zero, 0};
    };
    const auto mark_discard = [&](const std::uint64_t block) {
        const DataRange range{block * kM2BlockSize, kM2BlockSize};
        REQUIRE(store.value()->unmap(
            bounded_context(), {&range, 1}).ok());
        oracle[static_cast<std::size_t>(block)] =
            {OracleBlockKind::Discard, 0};
    };

    for (const auto block : std::array<std::uint64_t, 7>{
             0, 1, 31, 32, 33, block_count - 2, block_count - 1}) {
        write_data(block, static_cast<std::uint32_t>(0x100 + block));
    }
    for (const auto block : std::array<std::uint64_t, 3>{
             2, 34, block_count - 3}) {
        mark_zero(block);
    }
    for (const auto block : std::array<std::uint64_t, 3>{
             3, 35, block_count - 4}) {
        mark_discard(block);
    }
    write_data(4, 0x204);
    mark_zero(4);
    mark_zero(5);
    write_data(5, 0x205);
    mark_discard(6);
    write_data(6, 0x206);
    write_data(7, 0x207);
    mark_discard(7);

    const auto expected_data = static_cast<std::uint64_t>(std::count_if(
        oracle.begin(), oracle.end(), [](const OracleBlock& block) {
            return block.kind == OracleBlockKind::Data;
        }));
    const auto expected_zero = static_cast<std::uint64_t>(std::count_if(
        oracle.begin(), oracle.end(), [](const OracleBlock& block) {
            return block.kind == OracleBlockKind::Zero;
        }));
    const auto expected_discard = static_cast<std::uint64_t>(std::count_if(
        oracle.begin(), oracle.end(), [](const OracleBlock& block) {
            return block.kind == OracleBlockKind::Discard;
        }));

    auto writer_lock = acquire_bundle_writer_lock(
        sandbox.root(), create_request.manifest_relative_name);
    REQUIRE(writer_lock.ok());
    auto session = ImageSession::ready();
    CompactionCoordinator coordinator(session, *store.value());
    auto begun = coordinator.begin(bounded_context());
    REQUIRE(begun.ok());
    auto operation = begun.move_value();
    std::uint64_t offline_calls = 0;
    auto artifacts = make_artifact_request(
        writer_lock.value(), created.value(), offline_calls);
    REQUIRE(operation.build_artifacts(
        bounded_context(), artifacts).ok());
    auto journal_writer = make_compaction_journal_writer();
    auto prepared = coordinator.prepare_delta(
        bounded_context(), std::move(operation),
        make_delta_request(created.value()), *journal_writer);
    REQUIRE(prepared.ok());
    REQUIRE_EQ(prepared.transaction()->checkpoint().data_count,
        expected_data);
    REQUIRE_EQ(prepared.transaction()->checkpoint().zero_count,
        expected_zero);
    REQUIRE_EQ(prepared.transaction()->checkpoint().discard_count,
        expected_discard);
    auto transaction = prepared.move_transaction();

    std::vector<std::byte> post_cut(kM2BlockSize, std::byte{0xE7});
    REQUIRE(store.value()->write(
        bounded_context(), 0, post_cut).ok());
    const DataRange post_cut_zero{kM2BlockSize, kM2BlockSize};
    REQUIRE(store.value()->zero(
        bounded_context(), {&post_cut_zero, 1}).ok());

    auto manifest_capability = open_locked_manifest_for_mutation(
        writer_lock.value());
    REQUIRE(manifest_capability.ok());
    auto manifest_file = open_validated_persistence_file(
        manifest_capability.move_value());
    REQUIRE(manifest_file.ok());
    CheckpointManifestPublishRequest publish{};
    publish.manifest_file = manifest_file.move_value();
    publish.active_slot = created.value().selected_manifest;
    publish.committed_time_100ns = 445588;
    publish.complete_validation = CompleteCandidateValidation{
        sandbox.root(), create_request.manifest_relative_name,
        create_request.recovery};
    REQUIRE(transaction.publish_manifest(
        bounded_context(), std::move(publish)).ok());
    REQUIRE(transaction.reconcile(bounded_context()).ok());

    auto reopened = recover_complete_image(
        bounded_context(), sandbox.root(),
        create_request.manifest_relative_name,
        create_request.recovery);
    REQUIRE(reopened.ok());
    REQUIRE(reopened.image.has_value());
    REQUIRE_EQ(reopened.image->checkpoint_generation, 1ull);
    Sha256Stream expected_hash;
    Sha256Stream actual_hash;
    std::vector<std::byte> expected(kM2BlockSize);
    std::vector<std::byte> actual(kM2BlockSize);
    for (std::uint64_t block = 0; block < block_count; ++block) {
        std::fill(expected.begin(), expected.end(), std::byte{0});
        const auto& modeled = oracle[static_cast<std::size_t>(block)];
        if (modeled.kind == OracleBlockKind::Data) {
            fill_oracle_data(block, modeled.seed, expected);
        }
        std::fill(actual.begin(), actual.end(), std::byte{0xCC});
        auto loaded = reopened.image->backing_view.store->load_block(
            bounded_context(), block, actual);
        REQUIRE(loaded.ok());
        if (loaded.value().kind == BlockLoadKind::AllZero) {
            std::fill(actual.begin(), actual.end(), std::byte{0});
        } else {
            REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
        }
        REQUIRE_EQ(actual, expected);
        REQUIRE(expected_hash.update(expected).ok());
        REQUIRE(actual_hash.update(actual).ok());
    }
    const auto expected_digest = expected_hash.finalize_digest();
    const auto actual_digest = actual_hash.finalize_digest();
    REQUIRE(expected_digest.ok());
    REQUIRE(actual_digest.ok());
    REQUIRE_EQ(actual_digest.value(), expected_digest.value());
    REQUIRE(writer_lock.value().close().ok());
}

TEST_CASE(compaction_failure_matrix_never_recovers_a_mixed_generation) {
    for (const auto target : all_compaction_phases()) {
        CompactionSandbox sandbox;
        auto create_request = make_create_request(sandbox.root());
        auto created = create_generation_zero_image(
            bounded_context(), create_request);
        REQUIRE(created.ok());
        auto store = open_recovered_dynamic_store(
            created.value(), compaction_store_options());
        REQUIRE(store.ok());
        std::vector<std::byte> cut_data(kM2BlockSize, std::byte{0x5D});
        REQUIRE(store.value()->write(
            bounded_context(), 0, cut_data).ok());
        auto writer_lock = acquire_bundle_writer_lock(
            sandbox.root(), create_request.manifest_relative_name);
        REQUIRE(writer_lock.ok());
        const auto manifest_path =
            sandbox.root() / create_request.manifest_relative_name;
        const auto manifest_before = read_all_bytes(manifest_path);

        auto session = ImageSession::ready();
        CompactionCoordinator coordinator(session, *store.value());
        bool injected = false;
        std::vector<CompactionPhase> observed;
        auto begun = coordinator.begin_for_testing(
            bounded_context(),
            [&](const CompactionPhase phase) -> Result<void> {
                observed.push_back(phase);
                if (phase == target && !injected) {
                    injected = true;
                    return Result<void>::failure({
                        ErrorDomain::persistence,
                        static_cast<std::uint32_t>(
                            PersistenceErrorCode::IoError),
                        L"m3_compaction_tests",
                        L"injected compaction phase failure",
                        1117});
                }
                return Result<void>::success();
            });

        bool failure_observed = false;
        bool post_commit =
            static_cast<std::uint8_t>(target) >=
            static_cast<std::uint8_t>(
                CompactionPhase::ManifestPublishAfter);
        if (!begun.ok()) {
            failure_observed = true;
            REQUIRE(!post_commit);
        } else {
            auto operation = begun.move_value();
            std::uint64_t offline_calls = 0;
            auto artifacts = make_artifact_request(
                writer_lock.value(), created.value(), offline_calls);
            auto built = operation.build_artifacts(
                bounded_context(), artifacts);
            if (!built.ok()) {
                failure_observed = true;
                REQUIRE(!post_commit);
            } else {
                auto journal_writer = make_compaction_journal_writer();
                auto prepared = coordinator.prepare_delta(
                    bounded_context(), std::move(operation),
                    make_delta_request(created.value()), *journal_writer);
                if (!prepared.ok()) {
                    failure_observed = true;
                    REQUIRE(!post_commit);
                } else {
                    auto transaction = prepared.move_transaction();
                    auto manifest_capability =
                        open_locked_manifest_for_mutation(
                            writer_lock.value());
                    REQUIRE(manifest_capability.ok());
                    auto manifest_file = open_validated_persistence_file(
                        manifest_capability.move_value());
                    REQUIRE(manifest_file.ok());
                    CheckpointManifestPublishRequest publish{};
                    publish.manifest_file = manifest_file.move_value();
                    publish.active_slot =
                        created.value().selected_manifest;
                    publish.committed_time_100ns = 335577;
                    publish.complete_validation =
                        CompleteCandidateValidation{
                            sandbox.root(),
                            create_request.manifest_relative_name,
                            create_request.recovery};
                    const auto published = transaction.publish_manifest(
                        bounded_context(), std::move(publish));

                    if (!post_commit) {
                        failure_observed = !published.ok();
                        REQUIRE_EQ(published.disposition,
                            CommitDisposition::NotCommitted);
                    } else {
                        if (target ==
                                CompactionPhase::ManifestPublishAfter) {
                            REQUIRE(!published.ok());
                            REQUIRE_EQ(published.disposition,
                                CommitDisposition::
                                    CommittedNeedsReconciliation);
                            failure_observed = true;
                        } else {
                            REQUIRE(published.ok());
                            const auto first_reconcile =
                                transaction.reconcile(bounded_context());
                            REQUIRE(!first_reconcile.ok());
                            REQUIRE_EQ(first_reconcile.disposition,
                                CommitDisposition::
                                    CommittedNeedsReconciliation);
                            failure_observed = true;
                        }

                        auto recovery_during_failure =
                            recover_complete_image(
                                bounded_context(), sandbox.root(),
                                create_request.manifest_relative_name,
                                create_request.recovery);
                        REQUIRE(recovery_during_failure.ok());
                        REQUIRE(
                            recovery_during_failure.image.has_value());
                        REQUIRE_EQ(
                            recovery_during_failure.image
                                ->checkpoint_generation,
                            1ull);

                        const auto retried =
                            transaction.reconcile(bounded_context());
                        REQUIRE(retried.ok());
                    }
                }
            }
        }

        REQUIRE(injected);
        REQUIRE(failure_observed);
        REQUIRE(std::find(observed.begin(), observed.end(), target) !=
            observed.end());
        const auto manifest_after = read_all_bytes(manifest_path);
        auto recovered = recover_complete_image(
            bounded_context(), sandbox.root(),
            create_request.manifest_relative_name,
            create_request.recovery);
        REQUIRE(recovered.ok());
        REQUIRE(recovered.image.has_value());
        if (post_commit) {
            REQUIRE_EQ(recovered.image->checkpoint_generation, 1ull);
            REQUIRE(manifest_after != manifest_before);
            REQUIRE(std::filesystem::exists(
                sandbox.root() / create_request.base_relative_name));
            REQUIRE(std::filesystem::exists(
                sandbox.root() / create_request.journal_relative_name));
        } else {
            REQUIRE_EQ(recovered.image->checkpoint_generation, 0ull);
            REQUIRE_EQ(manifest_after, manifest_before);
            REQUIRE_EQ(store.value()->backing_generation(), 0ull);
        }
        REQUIRE_EQ(session.phase(), SessionPhase::Ready);
        REQUIRE(writer_lock.value().close().ok());
    }
}

}  // namespace
}  // namespace myrd::persistence
