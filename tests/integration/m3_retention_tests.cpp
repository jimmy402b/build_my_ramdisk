#include "src/persistence/retention_manager.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/recovered_store_adapter.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/image_session.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/vhdx_writer.h"
#include "src/persistence/writer_lock.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "src/storage/zero_backing_store.h"
#include "tests/support/test_framework.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <memory>
#include <filesystem>
#include <condition_variable>
#include <mutex>
#include <system_error>
#include <thread>

namespace {
using namespace myrd;
using namespace myrd::persistence;

template <std::size_t N>
std::array<std::byte, N> bytes(const std::uint8_t seed) {
    std::array<std::byte, N> value{};
    for (std::size_t index = 0; index < N; ++index) {
        value[index] = static_cast<std::byte>(seed + index);
    }
    return value;
}

OwnedImmutableArtifact base_candidate(std::string name = "old-base.vhdx") {
    OwnedImmutableArtifact value{};
    value.kind = ImmutableArtifactKind::BaseVhdx;
    value.relative_name = std::move(name);
    value.windows_identity.volume_serial_number = 7;
    value.windows_identity.file_id = bytes<16>(0x10);
    value.windows_identity.size = 6 * 1024 * 1024;
    value.windows_identity.hard_link_count = 1;
    value.image_uuid.bytes = bytes<16>(0x20);
    value.base_identity.file_write_guid = bytes<16>(0x30);
    value.base_identity.data_write_guid = bytes<16>(0x40);
    value.virtual_disk_id = bytes<16>(0x50);
    return value;
}

OwnedImmutableArtifact journal_candidate(
    std::string name = "old-journal.rdj") {
    auto value = base_candidate(std::move(name));
    value.kind = ImmutableArtifactKind::Journal;
    value.journal_identity.journal_uuid = bytes<16>(0x60);
    value.journal_epoch = 4;
    return value;
}

RetentionReferenceSnapshot references() {
    RetentionReferenceSnapshot value{};
    value.image_uuid.bytes = bytes<16>(0x20);
    value.virtual_disk_id = bytes<16>(0x50);
    return value;
}

DynamicBlockStoreOptions retention_store_options() {
    return {
        2 * kM2BlockSize,
        4 * kM2BlockSize,
        kM2BlockSize,
        kM2BlockSize,
        8,
        64,
        kMinimumSnapshotCowBytes,
        std::chrono::milliseconds{250}};
}

GenerationZeroCreateRequest production_create_request(
    const std::filesystem::path& root) {
    constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
    GenerationZeroCreateRequest request{};
    request.bundle_root = root;
    request.manifest_relative_name = "disk.rdm";
    request.base_temporary_name = "base.create.tmp";
    request.base_relative_name = "base-current.vhdx";
    request.journal_relative_name = "journal-current.rdj";
    request.geometry = {8 * kMiB, 512, 4096};
    request.image_uuid.bytes = bytes<16>(0x20);
    request.base_identity.file_identity.file_write_guid = bytes<16>(0x30);
    request.base_identity.file_identity.data_write_guid = bytes<16>(0x40);
    request.base_identity.virtual_disk_id = bytes<16>(0x50);
    request.journal_identity.journal_uuid = bytes<16>(0x70);
    request.journal_epoch = 1;
    request.operation_id = 3601;
    request.created_time_100ns = 123456;
    request.vhdx_limits = {4096, 4096, kVhdxDataBlockBytes};
    request.recovery.limits = {
        64 * kMiB, 64 * kMiB, 8192,
        64 * kIndexNodeBytes, 64,
        64 * kVhdxBatPageBytes, 64,
        2 * kMiB, 2 * kMiB};
    return request;
}

class RetentionSandbox final {
public:
    RetentionSandbox() {
        static std::uint64_t sequence{};
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-retention-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(++sequence));
        std::error_code error;
        REQUIRE(std::filesystem::create_directory(root_, error));
        REQUIRE(!error);
    }
    ~RetentionSandbox() {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }
    const std::filesystem::path& root() const noexcept { return root_; }
private:
    std::filesystem::path root_;
};

JournalHeader owned_header() {
    JournalHeader header{};
    header.image_uuid.bytes = bytes<16>(0x20);
    header.journal_identity.journal_uuid = bytes<16>(0x60);
    header.epoch = 1;
    header.geometry = {64ULL * 1024 * 1024, 512, 4096};
    VhdxFileIdentity base{};
    base.file_write_guid = bytes<16>(0x30);
    base.data_write_guid = bytes<16>(0x40);
    auto geometry = compute_geometry_digest(header.geometry);
    auto base_digest = compute_base_identity_digest(base);
    REQUIRE(geometry.ok());
    REQUIRE(base_digest.ok());
    header.geometry_digest = geometry.value();
    header.base_identity_digest = base_digest.value();
    auto genesis = compute_genesis_digest(
        header.image_uuid, header.geometry_digest,
        header.base_identity_digest);
    REQUIRE(genesis.ok());
    header.predecessor_digest = genesis.value();
    return header;
}

OwnedImmutableArtifact create_owned_journal(
    BundleWriterLock& writer,
    const std::filesystem::path& root,
    const std::string& name = "old-journal.rdj") {
    const auto header = owned_header();
    auto capability = create_bundle_file_for_mutation(writer, name);
    REQUIRE(capability.ok());
    auto opened = open_validated_persistence_file(capability.move_value());
    REQUIRE(opened.ok());
    auto file = opened.move_value();
    auto encoded = encode_journal_header(header);
    REQUIRE(encoded.ok());
    REQUIRE(file->truncate(OperationContext{}, kJournalAlignment).ok());
    REQUIRE(file->write_exact(OperationContext{}, 0, encoded.value()).ok());
    REQUIRE(file->flush(OperationContext{}).ok());
    REQUIRE(file->close().ok());
    auto inspection = open_bundle_file_for_inspection(root, name);
    REQUIRE(inspection.ok());

    OwnedImmutableArtifact candidate{};
    candidate.kind = ImmutableArtifactKind::Journal;
    candidate.relative_name = name;
    candidate.windows_identity = inspection.value().identity();
    candidate.image_uuid = header.image_uuid;
    candidate.journal_identity = header.journal_identity;
    candidate.journal_epoch = header.epoch;
    return candidate;
}

RetentionReferenceSnapshot references_for(
    const OwnedImmutableArtifact& candidate) {
    auto value = references();
    value.image_uuid = candidate.image_uuid;
    return value;
}

Result<RetentionCleanupReport> run_cleanup(
    const OperationContext& context,
    BundleWriterLock& writer,
    const std::filesystem::path& root,
    const RetentionReferenceSnapshot& refs,
    const std::span<const OwnedImmutableArtifact> candidates,
    const RetentionCleanupOptions options) {
    auto session = ImageSession::ready();
    auto lease = session.begin_cleanup();
    REQUIRE(lease.ok());
    return myrd::persistence::test::cleanup_retention_artifacts_with_snapshot(
        context, lease.move_value(), writer, root, refs, candidates, options);
}

class EmptyPayloadSource final : public IVhdxPayloadSource {
public:
    Result<void> read_payload(
        const OperationContext&,
        std::uint64_t,
        std::span<std::byte>) const override {
        return Result<void>::failure({
            ErrorDomain::internal, 1, L"retention.empty_source",
            L"empty VHDX requested payload data"});
    }
};

class EmptyJournalLayer final : public detail::ICompositeJournalLayer {
public:
    Result<detail::CompositeJournalRead> read_block(
        const OperationContext&, std::uint64_t,
        std::span<std::byte>) const override {
        return Result<detail::CompositeJournalRead>::success(
            {detail::CompositeJournalKind::Absent});
    }
    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext&, std::uint64_t) const override {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
};

class EmptyBaseLayer final : public detail::ICompositeBaseLayer {
public:
    Result<detail::CompositeBaseRead> read_block(
        const OperationContext&, std::uint64_t,
        std::span<std::byte>) const override {
        return Result<detail::CompositeBaseRead>::success(
            {detail::CompositeBaseKind::Zero});
    }
    Result<std::optional<std::uint64_t>> next_candidate_block(
        const OperationContext&, std::uint64_t) const override {
        return Result<std::optional<std::uint64_t>>::success(std::nullopt);
    }
};

OwnedImmutableArtifact create_owned_base(
    BundleWriterLock& writer,
    const std::string& name = "old-base.vhdx") {
    VhdxWriterIdentity identity{};
    identity.file_identity.file_write_guid = bytes<16>(0x30);
    identity.file_identity.data_write_guid = bytes<16>(0x40);
    identity.virtual_disk_id = bytes<16>(0x50);
    EmptyPayloadSource source;
    const std::vector<std::uint64_t> candidates;
    auto plan = plan_sparse_vhdx(
        OperationContext{},
        {kLogicalSectorSize512, kLogicalSectorSize512}, identity,
        candidates, source, {4096, 4096, kVhdxDataBlockBytes});
    REQUIRE(plan.ok());

    OwnedImmutableArtifact candidate{};
    {
        auto published = publish_sparse_vhdx_base(
            OperationContext{}, writer, name + ".tmp", name,
            plan.value(), source);
        REQUIRE(published.ok());
        candidate.kind = ImmutableArtifactKind::BaseVhdx;
        candidate.relative_name = name;
        candidate.windows_identity = published.value().windows_identity();
        candidate.image_uuid.bytes = bytes<16>(0x20);
        candidate.base_identity = identity.file_identity;
        candidate.virtual_disk_id = identity.virtual_disk_id;
    }
    return candidate;
}

TEST_CASE(retention_pure_decision_rejects_unsafe_candidates) {
    auto refs = references();
    auto candidate = base_candidate();
    REQUIRE_EQ(evaluate_retention_candidate(candidate, refs),
               RetentionDecision::Eligible);

    candidate.relative_name = "..\\escape.vhdx";
    REQUIRE_EQ(evaluate_retention_candidate(candidate, refs),
               RetentionDecision::InvalidName);
    candidate = base_candidate();
    candidate.windows_identity.attributes = FILE_ATTRIBUTE_REPARSE_POINT;
    REQUIRE_EQ(evaluate_retention_candidate(candidate, refs),
               RetentionDecision::ReparsePoint);
    candidate = base_candidate();
    candidate.windows_identity.hard_link_count = 2;
    REQUIRE_EQ(evaluate_retention_candidate(candidate, refs),
               RetentionDecision::HardLinked);
    candidate = base_candidate();
    candidate.image_uuid.bytes[0] = std::byte{0xff};
    REQUIRE_EQ(evaluate_retention_candidate(candidate, refs),
               RetentionDecision::UnknownOwnership);
    candidate = base_candidate();
    candidate.virtual_disk_id[0] = std::byte{0xff};
    REQUIRE_EQ(evaluate_retention_candidate(candidate, refs),
               RetentionDecision::UnknownOwnership);
}

TEST_CASE(retention_accounts_for_both_manifest_slots_case_insensitively) {
    auto refs = references();
    ManifestSlot slot_a{};
    slot_a.base_relative_name = "OLD-BASE.VHDX";
    slot_a.journal_relative_name = "current-a.rdj";
    ManifestSlot slot_b{};
    slot_b.base_relative_name = "current-b.vhdx";
    slot_b.journal_relative_name = "OLD-JOURNAL.RDJ";
    refs.valid_slots[0] = slot_a;
    refs.valid_slots[1] = slot_b;

    REQUIRE_EQ(evaluate_retention_candidate(base_candidate(), refs),
               RetentionDecision::ManifestReferenced);
    REQUIRE_EQ(evaluate_retention_candidate(journal_candidate(), refs),
               RetentionDecision::ManifestReferenced);
}

TEST_CASE(retention_holds_live_view_until_the_last_reader_releases) {
    auto refs = references();
    auto lifetime = std::shared_ptr<const IBackingStore>{
        std::make_shared<ZeroBackingStore>()};
    RetentionLiveViewReference live{};
    live.identity.base_windows_identity = base_candidate().windows_identity;
    live.identity.journal_windows_identity =
        journal_candidate().windows_identity;
    live.lifetime = lifetime;
    refs.live_views.push_back(live);

    REQUIRE_EQ(evaluate_retention_candidate(base_candidate(), refs),
               RetentionDecision::LiveViewReferenced);
    REQUIRE_EQ(evaluate_retention_candidate(journal_candidate(), refs),
               RetentionDecision::LiveViewReferenced);
    lifetime.reset();
    REQUIRE_EQ(evaluate_retention_candidate(base_candidate(), refs),
               RetentionDecision::Eligible);
}

TEST_CASE(retention_accounts_for_inflight_publish_and_reconcile_ownership) {
    auto refs = references();
    refs.in_flight_artifacts.push_back(base_candidate("OLD-BASE.VHDX"));
    REQUIRE_EQ(evaluate_retention_candidate(base_candidate(), refs),
               RetentionDecision::InFlightReferenced);

    auto replacement = base_candidate();
    replacement.windows_identity.file_id[0] = std::byte{0xee};
    refs.in_flight_artifacts[0] = replacement;
    REQUIRE_EQ(evaluate_retention_candidate(base_candidate(), refs),
               RetentionDecision::Eligible);
}

TEST_CASE(retention_dry_run_reports_relative_names_without_touching_files) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    const std::array candidates{candidate};

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, true});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().eligible_count, std::uint64_t{1});
    REQUIRE_EQ(report.value().delete_attempt_count, std::uint64_t{0});
    REQUIRE_EQ(report.value().entries.size(), std::size_t{1});
    REQUIRE_EQ(report.value().entries[0].relative_name,
               std::string("old-journal.rdj"));
    REQUIRE(std::filesystem::exists(sandbox.root() / L"old-journal.rdj"));
}

TEST_CASE(retention_deletes_only_after_handle_anchored_format_revalidation) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    const std::array candidates{candidate};

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, false});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().delete_attempt_count, std::uint64_t{1});
    REQUIRE_EQ(report.value().deleted_count, std::uint64_t{1});
    REQUIRE_EQ(report.value().entries[0].action,
               RetentionCleanupAction::Deleted);
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"old-journal.rdj"));
}

TEST_CASE(retention_sharing_violation_is_harmless_and_recoverable) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    const auto path = sandbox.root() / L"old-journal.rdj";
    auto blocker_result = open_bundle_file_for_backing_view(
        sandbox.root(), candidate.relative_name,
        candidate.windows_identity);
    REQUIRE(blocker_result.ok());
    std::optional<OpenedBackingViewFile> blocker{
        blocker_result.move_value()};
    auto blocker_file_result = open_validated_persistence_file(
        blocker->take_file());
    REQUIRE(blocker_file_result.ok());
    auto blocker_file = blocker_file_result.move_value();
    const std::array candidates{candidate};

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, false});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().deleted_count, std::uint64_t{0});
    REQUIRE(report.value().entries[0].has_error);
    REQUIRE_EQ(report.value().entries[0].native_code,
               static_cast<std::uint32_t>(ERROR_SHARING_VIOLATION));
    REQUIRE(std::filesystem::exists(path));

    // Closing every reader alias must not release Composite's independent
    // namespace-retention anchor.
    REQUIRE(blocker_file->close().ok());
    blocker_file.reset();
    auto still_guarded = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, false});
    REQUIRE(still_guarded.ok());
    REQUIRE_EQ(still_guarded.value().deleted_count, std::uint64_t{0});
    REQUIRE_EQ(still_guarded.value().entries[0].native_code,
               static_cast<std::uint32_t>(ERROR_SHARING_VIOLATION));

    blocker.reset();
    auto retried = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, false});
    REQUIRE(retried.ok());
    REQUIRE_EQ(retried.value().deleted_count, std::uint64_t{1});
}

TEST_CASE(retention_replaced_or_hardlinked_candidate_is_never_deleted) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    const auto path = sandbox.root() / L"old-journal.rdj";
    const auto alias = sandbox.root() / L"alias.rdj";
    REQUIRE(CreateHardLinkW(alias.c_str(), path.c_str(), nullptr));
    const std::array candidates{candidate};

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, false});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().deleted_count, std::uint64_t{0});
    REQUIRE(report.value().entries[0].has_error);
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::exists(alias));
}

TEST_CASE(retention_revalidates_and_deletes_an_owned_vhdx_base) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_base(writer);
    const std::array candidates{candidate};

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, false});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().deleted_count, std::uint64_t{1});
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"old-base.vhdx"));
}

TEST_CASE(retention_never_calls_delete_until_the_last_live_view_releases) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    auto refs = references_for(candidate);
    auto lifetime = std::shared_ptr<const IBackingStore>{
        std::make_shared<ZeroBackingStore>()};
    RetentionLiveViewReference live{};
    live.identity.journal_windows_identity = candidate.windows_identity;
    live.lifetime = lifetime;
    refs.live_views.push_back(live);
    const std::array candidates{candidate};

    auto retained = run_cleanup(
        OperationContext{}, writer, sandbox.root(), refs, candidates,
        {16, false});
    REQUIRE(retained.ok());
    REQUIRE_EQ(retained.value().delete_attempt_count, std::uint64_t{0});
    REQUIRE(std::filesystem::exists(
        sandbox.root() / L"old-journal.rdj"));

    lifetime.reset();
    auto deleted = run_cleanup(
        OperationContext{}, writer, sandbox.root(), refs, candidates,
        {16, false});
    REQUIRE(deleted.ok());
    REQUIRE_EQ(deleted.value().deleted_count, std::uint64_t{1});
}

TEST_CASE(retention_toctou_replacement_keeps_the_new_file) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    const auto path = sandbox.root() / L"old-journal.rdj";
    const auto retired = sandbox.root() / L"retired.rdj";
    REQUIRE(MoveFileExW(
        path.c_str(), retired.c_str(), MOVEFILE_WRITE_THROUGH));
    const auto replacement = create_owned_journal(
        writer, sandbox.root(), "old-journal.rdj");
    REQUIRE(!same_file_object(
        candidate.windows_identity, replacement.windows_identity));
    const std::array candidates{candidate};

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, false});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().deleted_count, std::uint64_t{0});
    REQUIRE(report.value().entries[0].has_error);
    REQUIRE(std::filesystem::exists(path));
    REQUIRE(std::filesystem::exists(retired));
}

TEST_CASE(retention_corrupt_owned_artifact_is_retained_with_diagnostics) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    const auto path = sandbox.root() / L"old-journal.rdj";
    UniqueHandle corrupt(CreateFileW(
        path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    REQUIRE(static_cast<bool>(corrupt));
    const std::array<std::byte, 8> bad{
        std::byte{'b'}, std::byte{'a'}, std::byte{'d'}, std::byte{'!'},
        std::byte{'b'}, std::byte{'a'}, std::byte{'d'}, std::byte{'!'}};
    DWORD written{};
    REQUIRE(WriteFile(
        corrupt.get(), bad.data(), static_cast<DWORD>(bad.size()),
        &written, nullptr));
    REQUIRE_EQ(written, static_cast<DWORD>(bad.size()));
    REQUIRE(FlushFileBuffers(corrupt.get()));
    corrupt.reset();
    const std::array candidates{candidate};

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, {16, false});
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().delete_attempt_count, std::uint64_t{0});
    REQUIRE(report.value().entries[0].has_error);
    REQUIRE(std::filesystem::exists(path));
}

TEST_CASE(retention_candidate_budget_fails_before_any_mutation) {
    auto first = base_candidate("one.vhdx");
    auto second = base_candidate("two.vhdx");
    second.windows_identity.file_id[0] = std::byte{0x77};
    const std::array candidates{first, second};
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references(),
        candidates, {1, true});
    REQUIRE(!report.ok());
    REQUIRE_EQ(report.error().code, static_cast<std::uint32_t>(
        PersistenceErrorCode::SnapshotResourceExhausted));
}

TEST_CASE(retention_cleanup_lease_excludes_compaction_and_releases_to_ready) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    auto session = ImageSession::ready();
    auto reader = session.begin_immutable_read();
    REQUIRE(reader.ok());
    auto blocked_by_reader = session.begin_cleanup();
    REQUIRE(!blocked_by_reader.ok());
    reader.value().release();
    auto lease = session.begin_cleanup();
    REQUIRE(lease.ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Verifying);
    REQUIRE(!session.begin_immutable_read().ok());
    REQUIRE(!lease.value().note_publish_attempt_started().ok());
    auto blocked = session.begin_compaction();
    REQUIRE(!blocked.ok());
    const auto candidate = base_candidate();
    const std::array candidates{candidate};

    auto report =
        myrd::persistence::test::cleanup_retention_artifacts_with_snapshot(
        OperationContext{}, lease.move_value(), writer, sandbox.root(),
        references(), candidates, {16, true});
    REQUIRE(report.ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Ready);
    auto compaction = session.begin_compaction();
    REQUIRE(compaction.ok());
    REQUIRE(compaction.value().abort().ok());
}

TEST_CASE(retention_store_tracks_captured_retired_view_until_last_release) {
    auto old_backing = std::make_shared<ZeroBackingStore>();
    auto created = DynamicBlockStore::create({
        4 * kM2BlockSize,
        512,
        retention_store_options(),
        InitialBlockState::BackingUnresident,
        old_backing,
        1});
    REQUIRE(created.ok());
    auto store = created.move_value();
    auto held_old = store->capture_backing_view();
    auto next_backing = std::make_shared<ZeroBackingStore>();
    REQUIRE(store->install_backing_view({2, next_backing}).ok());

    {
        auto live = store->capture_live_backing_views(2);
        REQUIRE(live.ok());
        REQUIRE_EQ(live.value().size(), std::size_t{2});
        auto bounded = store->capture_live_backing_views(1);
        REQUIRE(!bounded.ok());
    }
    held_old.store.reset();
    old_backing.reset();
    auto released = store->capture_live_backing_views(1);
    REQUIRE(released.ok());
    REQUIRE_EQ(released.value().size(), std::size_t{1});
    REQUIRE_EQ(released.value()[0].generation, BackingGeneration{2});
}

TEST_CASE(retention_product_path_captures_authoritative_manifest_references) {
    RetentionSandbox sandbox;
    const auto request = production_create_request(sandbox.root());
    auto created = create_generation_zero_image(
        OperationContext{}, request);
    REQUIRE(created.ok());
    auto store = open_recovered_dynamic_store(
        created.value(), retention_store_options());
    REQUIRE(store.ok());
    auto writer_result = acquire_bundle_writer_lock(
        sandbox.root(), request.manifest_relative_name);
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(
        writer, sandbox.root(), "journal-obsolete.rdj");
    const std::array candidates{candidate};
    auto retired_identity = created.value().backing_identity;
    retired_identity.journal_windows_identity = candidate.windows_identity;
    retired_identity.journal_identity = candidate.journal_identity;
    retired_identity.journal_epoch = candidate.journal_epoch;
    auto retired_store_result = CompositeBackingStore::create_for_testing(
        retired_identity, std::make_unique<EmptyJournalLayer>(),
        std::make_unique<EmptyBaseLayer>());
    REQUIRE(retired_store_result.ok());
    std::shared_ptr<const IBackingStore> retired_store(
        retired_store_result.move_value());
    BackingView retired_view{
        retired_identity.checkpoint_generation, retired_store};
    REQUIRE(DynamicBlockStoreTestAccess::
        track_retired_backing_view_for_test(
            *store.value(), retired_view.generation,
            retired_view.store));
    auto session = ImageSession::ready();
    auto lease = session.begin_cleanup();
    REQUIRE(lease.ok());
    RetentionCleanupOptions options{16, false};
    options.max_live_views = 16;
    options.recovery = request.recovery;

    auto retained = cleanup_retention_artifacts(
        OperationContext{}, lease.move_value(), writer, *store.value(),
        sandbox.root(),
        request.manifest_relative_name, candidates, options);

    REQUIRE(retained.ok());
    REQUIRE_EQ(retained.value().deleted_count, std::uint64_t{0});
    REQUIRE_EQ(retained.value().entries[0].decision,
               RetentionDecision::LiveViewReferenced);
    REQUIRE(std::filesystem::exists(
        sandbox.root() / L"journal-obsolete.rdj"));

    retired_view.store.reset();
    retired_store.reset();
    auto second_lease = session.begin_cleanup();
    REQUIRE(second_lease.ok());
    auto deleted = cleanup_retention_artifacts(
        OperationContext{}, second_lease.move_value(), writer,
        *store.value(), sandbox.root(), request.manifest_relative_name,
        candidates, options);
    REQUIRE(deleted.ok());
    REQUIRE_EQ(deleted.value().deleted_count, std::uint64_t{1});
    REQUIRE(!std::filesystem::exists(
        sandbox.root() / L"journal-obsolete.rdj"));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / request.base_relative_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / request.journal_relative_name));
    REQUIRE(std::filesystem::exists(
        sandbox.root() / request.manifest_relative_name));
}

TEST_CASE(retention_strong_pin_closes_content_toctou_before_validation) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    const auto path = sandbox.root() / L"old-journal.rdj";
    DWORD observed_error = ERROR_SUCCESS;
    std::uint32_t observed_backing_error{};
    RetentionCleanupOptions options{16, false};
    options.phase_hook = [&](const RetentionCleanupTestPhase phase) {
        if (phase !=
            RetentionCleanupTestPhase::AfterProtectionBeforeValidation) {
            return Result<void>::success();
        }
        UniqueHandle writer_handle(CreateFileW(
            path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (writer_handle) {
            return Result<void>::failure({
                ErrorDomain::internal, 1, L"retention.toctou",
                L"content writer unexpectedly opened through cleanup pin"});
        }
        observed_error = GetLastError();
        auto backing = open_bundle_file_for_backing_view(
            sandbox.root(), candidate.relative_name,
            candidate.windows_identity);
        if (backing.ok()) {
            return Result<void>::failure({
                ErrorDomain::internal, 2, L"retention.toctou",
                L"backing view unexpectedly opened through delete pin"});
        }
        observed_backing_error = backing.error().native_code;
        return Result<void>::success();
    };
    const std::array candidates{candidate};

    auto report = run_cleanup(
        OperationContext{}, writer, sandbox.root(), references_for(candidate),
        candidates, options);
    REQUIRE(report.ok());
    REQUIRE_EQ(observed_error,
               static_cast<DWORD>(ERROR_SHARING_VIOLATION));
    REQUIRE_EQ(observed_backing_error,
               static_cast<std::uint32_t>(ERROR_SHARING_VIOLATION));
    REQUIRE_EQ(report.value().deleted_count, std::uint64_t{1});
}

TEST_CASE(retention_session_close_cancels_before_destructive_boundary) {
    RetentionSandbox sandbox;
    auto writer_result = create_bundle_manifest_and_acquire_writer(
        sandbox.root(), "disk.rdm");
    REQUIRE(writer_result.ok());
    auto writer = writer_result.move_value();
    const auto candidate = create_owned_journal(writer, sandbox.root());
    const std::array candidates{candidate};
    auto session = ImageSession::ready();
    auto lease = session.begin_cleanup();
    REQUIRE(lease.ok());
    const auto cleanup_stop = lease.value().stop_token();
    std::mutex mutex;
    std::condition_variable condition;
    bool at_boundary = false;
    bool release_boundary = false;
    RetentionCleanupOptions options{16, false};
    options.phase_hook = [&](const RetentionCleanupTestPhase phase) {
        if (phase != RetentionCleanupTestPhase::BeforeDelete) {
            return Result<void>::success();
        }
        std::unique_lock lock(mutex);
        at_boundary = true;
        condition.notify_all();
        condition.wait(lock, [&] { return release_boundary; });
        return Result<void>::success();
    };
    std::optional<Result<RetentionCleanupReport>> cleanup_result;
    std::optional<Result<SessionCloseOutcome>> close_result;
    std::thread cleanup([&] {
        cleanup_result.emplace(
            myrd::persistence::test::
                cleanup_retention_artifacts_with_snapshot(
                    OperationContext{}, lease.move_value(), writer,
                    sandbox.root(), references_for(candidate),
                    candidates, options));
    });
    {
        std::unique_lock lock(mutex);
        condition.wait(lock, [&] { return at_boundary; });
    }
    std::thread closer([&] {
        close_result.emplace(session.close(OperationContext{}));
    });
    while (!cleanup_stop.stop_requested()) {
        std::this_thread::yield();
    }
    {
        std::lock_guard lock(mutex);
        release_boundary = true;
    }
    condition.notify_all();
    cleanup.join();
    closer.join();

    REQUIRE(cleanup_result.has_value());
    REQUIRE(!cleanup_result->ok());
    REQUIRE(close_result.has_value());
    REQUIRE(close_result->ok());
    REQUIRE_EQ(session.phase(), SessionPhase::Closed);
    REQUIRE(std::filesystem::exists(
        sandbox.root() / L"old-journal.rdj"));
}

} // namespace
