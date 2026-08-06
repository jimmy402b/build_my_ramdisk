#include "test_framework.h"

#include "src/m3/m3_report.h"
#include "src/m3/m3_evidence_publisher.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/image_inspector.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/manifest_format.h"
#include "src/persistence/vhdx_format.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/writer_lock.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

class InspectSandbox final {
public:
    InspectSandbox() {
        static std::atomic<std::uint64_t> sequence{};
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-m3-inspect-" +
             std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(root_);
    }
    ~InspectSandbox() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    const std::filesystem::path& root() const noexcept { return root_; }
private:
    std::filesystem::path root_{};
};

myrd::persistence::GenerationZeroCreateRequest create_request(
    const std::filesystem::path& root) {
    using namespace myrd::persistence;
    constexpr std::uint64_t mib = 1024ULL * 1024ULL;
    GenerationZeroCreateRequest request{};
    request.bundle_root = root;
    request.manifest_relative_name = "disk.rdm";
    request.base_temporary_name = "base.create.tmp";
    request.base_relative_name = "base-0000.vhdx";
    request.journal_relative_name = "journal-e0001.rdj";
    request.geometry = {8 * mib, 512, 4096};
    request.image_uuid.bytes[0] = std::byte{0x11};
    request.base_identity.file_identity.file_write_guid[0] =
        std::byte{0x21};
    request.base_identity.file_identity.data_write_guid[0] =
        std::byte{0x22};
    request.base_identity.virtual_disk_id[0] = std::byte{0x23};
    request.journal_identity.journal_uuid[0] = std::byte{0x31};
    request.journal_epoch = 1;
    request.operation_id = 3701;
    request.created_time_100ns = 123456;
    request.vhdx_limits = {4096, 4096, kVhdxDataBlockBytes};
    request.recovery.limits = {
        64 * mib, 64 * mib, 8192,
        64 * kIndexNodeBytes, 64,
        64 * kVhdxBatPageBytes, 64,
        2 * mib, 2 * mib};
    return request;
}

std::vector<char> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

std::uint64_t publish_mixed_checkpoint(
    const std::filesystem::path& root,
    myrd::persistence::ManifestSlot active) {
    using namespace myrd::persistence;
    const auto first_record_offset =
        active.generation.journal.committed_length;
    auto lock = acquire_bundle_writer_lock(root, "disk.rdm");
    REQUIRE(lock.ok());
    auto journal_capability = open_bundle_file_for_mutation(
        lock.value(), active.journal_relative_name);
    REQUIRE(journal_capability.ok());
    const auto journal_identity = journal_capability.value().identity();
    auto journal_file = open_validated_persistence_file(
        journal_capability.move_value());
    REQUIRE(journal_file.ok());
    auto shared_journal = std::shared_ptr<IPersistenceFile>(
        journal_file.move_value());
    std::array<std::byte, kJournalAlignment> header_bytes{};
    REQUIRE(shared_journal->read_exact(
        myrd::OperationContext{}, 0, header_bytes).ok());
    auto header = decode_journal_header(header_bytes);
    REQUIRE(header.ok());

    std::array<std::byte, kM2BlockSize> payload{};
    payload.fill(std::byte{0x5A});
    const std::array<JournalCheckpointBlockInput, 3> blocks{{
        {JournalRecordType::Data, 0, 1, 0, 1, payload},
        {JournalRecordType::Zero, 1, 1, 0, 1, {}},
        {JournalRecordType::Discard, 2, 1, 0, 1, {}}}};
    JournalCheckpointRequest checkpoint{};
    checkpoint.file = shared_journal;
    checkpoint.file_identity = journal_identity;
    checkpoint.header = header.value();
    checkpoint.previous_generation =
        active.generation.checkpoint_generation;
    checkpoint.previous_digest = active.generation.journal.commit.chain_digest;
    checkpoint.committed_length = active.generation.journal.committed_length;
    checkpoint.root = active.generation.journal.root;
    checkpoint.operation_id = 3702;
    checkpoint.blocks = blocks;
    JournalWriterOptions writer_options{};
    writer_options.index_builder = {
        {64 * kIndexNodeBytes, 64}, 4096, 4096 * kIndexNodeBytes};
    writer_options.max_checkpoint_records = 8192;
    writer_options.max_checkpoint_bytes = 64ULL * 1024ULL * 1024ULL;
    auto writer = JournalWriter::create(writer_options);
    REQUIRE(writer.ok());
    auto prepared = writer.value()->prepare(
        myrd::OperationContext{}, checkpoint);
    REQUIRE(prepared.ok());

    active.slot_index = 1;
    active.generation.checkpoint_generation = prepared.value().generation;
    active.generation.journal.committed_length =
        prepared.value().committed_length;
    active.generation.journal.root = prepared.value().root;
    active.generation.journal.commit = prepared.value().commit;
    ++active.committed_time_100ns;
    auto encoded = encode_manifest_slot(active);
    REQUIRE(encoded.ok());
    auto manifest_capability = open_locked_manifest_for_mutation(lock.value());
    REQUIRE(manifest_capability.ok());
    auto manifest_file = open_validated_persistence_file(
        manifest_capability.move_value());
    REQUIRE(manifest_file.ok());
    REQUIRE(manifest_file.value()->write_exact(
        myrd::OperationContext{}, kManifestSlotBytes, encoded.value()).ok());
    REQUIRE(manifest_file.value()->flush(myrd::OperationContext{}).ok());
    manifest_file.value().reset();
    checkpoint.file.reset();
    shared_journal.reset();
    REQUIRE(lock.value().close().ok());
    return first_record_offset;
}

myrd::m3::M3Report complete_report() {
    using namespace myrd::m3;
    M3Report report{};
    report.operation = ReportOperation::VerifyFull;
    report.status = ReportStatus::Passed;
    report.operation_id = "op-0001";
    report.started_utc = "2026-08-06T00:00:00.000Z";
    report.finished_utc = "2026-08-06T00:00:01.000Z";
    report.build.source_commit = "0123456789abcdef0123456789abcdef01234567";
    report.build.build_type = "Debug";
    report.build.binary_sha256 =
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    report.bundle_path_hash =
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB";
    report.image = ImageEvidence{
        "00112233445566778899AABBCCDDEEFF",
        64ULL * 1024ULL * 1024ULL, 512, 4096};
    report.slots[0] = SlotEvidence{
        "A", SlotStatus::Selected, 7, std::nullopt, "passed", true};
    report.slots[1] = SlotEvidence{
        "B", SlotStatus::FallbackRejected, 8, "journal_digest",
        "corrupt", false};
    report.selected = SelectedGenerationEvidence{
        0, true, 7, 3, 4,
        "base-windows-id", "base-vhdx-id", "journal-windows-id",
        "journal-format-id", 12288,
        DescriptorEvidence{4096, 4096, 17},
        CommitEvidence{8192, 4096, 23,
            "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"}};
    report.verification = VerificationEvidence{
        VerificationDepth::Full, true, 9, 7, 2,
        7ULL * 64ULL * 1024ULL,
        "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"};
    report.cleanup_candidates.push_back(
        {"base", "base-g1.vhdx", "ManifestReferenced", "Kept"});
    report.artifacts.push_back({
        "manifest",
        "EEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEEE"});
    return report;
}

myrd::m3::M3ReportContext report_context() {
    myrd::m3::M3ReportContext context{};
    context.operation_id = "op-3701";
    context.started_utc = "2026-08-06T00:00:00.000Z";
    context.finished_utc = "2026-08-06T00:00:01.000Z";
    context.build.source_commit =
        "0123456789abcdef0123456789abcdef01234567";
    context.build.build_type = "Debug";
    context.build.binary_sha256 = std::string(64, 'A');
    context.bundle_path_hash = std::string(64, 'B');
    context.artifacts.push_back({"manifest", std::string(64, 'C')});
    return context;
}

} // namespace

TEST_CASE(m3_report_schema_is_deterministic_and_ordered) {
    const auto report = complete_report();
    const auto first = myrd::m3::serialize_m3_report(report);
    const auto second = myrd::m3::serialize_m3_report(report);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE_EQ(first.value(), second.value());

    std::size_t cursor = 0;
    for (const auto* key : {
             "schema_version", "operation", "status", "operation_id",
             "started_utc", "finished_utc", "build", "bundle_path_hash",
             "bundle_path", "image", "slots", "selected", "verification",
             "cleanup_candidates", "artifacts", "error"}) {
        const auto position = first.value().find(
            std::string{"\""} + key + "\"", cursor);
        REQUIRE(position != std::string::npos);
        cursor = position + 1;
    }
    REQUIRE(first.value().find("\"operation\": \"verify_full\"") !=
            std::string::npos);
    REQUIRE(first.value().find("\"status\": \"passed\"") !=
            std::string::npos);
    REQUIRE(first.value().find("\"status\": \"fallback_rejected\"") !=
            std::string::npos);
    REQUIRE(first.value().find(",\n}") == std::string::npos);
    REQUIRE(first.value().find(",\n]") == std::string::npos);
}

TEST_CASE(m3_report_redacts_paths_and_never_serializes_block_contents) {
    auto report = complete_report();
    report.bundle_path = "C:\\Users\\JimmyLin\\secret\\disk.myrd";
    report.include_absolute_path = false;
    const auto redacted = myrd::m3::serialize_m3_report(report);
    REQUIRE(redacted.ok());
    REQUIRE(redacted.value().find("JimmyLin") == std::string::npos);
    REQUIRE(redacted.value().find("disk.myrd") == std::string::npos);
    REQUIRE(redacted.value().find("\"bundle_path\": null") !=
            std::string::npos);

    report.include_absolute_path = true;
    const auto diagnostic = myrd::m3::serialize_m3_report(report);
    REQUIRE(diagnostic.ok());
    REQUIRE(diagnostic.value().find("JimmyLin") != std::string::npos);
    REQUIRE(diagnostic.value().find("block_contents") == std::string::npos);
}

TEST_CASE(m3_report_fails_closed_on_required_identity_or_unknown_enum) {
    auto report = complete_report();
    report.build.source_commit.clear();
    REQUIRE(!myrd::m3::serialize_m3_report(report).ok());

    report = complete_report();
    report.operation = static_cast<myrd::m3::ReportOperation>(255);
    REQUIRE(!myrd::m3::serialize_m3_report(report).ok());

    report = complete_report();
    report.artifacts[0].sha256 = "not-a-hash";
    REQUIRE(!myrd::m3::serialize_m3_report(report).ok());

    report = complete_report();
    report.operation_id = std::string{"\xC3\x28", 2};
    REQUIRE(!myrd::m3::serialize_m3_report(report).ok());

    report = complete_report();
    report.include_absolute_path = true;
    report.bundle_path = "relative\\disk.myrd";
    REQUIRE(!myrd::m3::serialize_m3_report(report).ok());

    report = complete_report();
    report.status = myrd::m3::ReportStatus::Failed;
    report.error = myrd::m3::ErrorEvidence{
        "unknown", 1, 0, "verify", "bad"};
    REQUIRE(!myrd::m3::serialize_m3_report(report).ok());
}

TEST_CASE(m3_report_preserves_no_selection_and_corruption_provenance) {
    auto report = complete_report();
    report.status = myrd::m3::ReportStatus::Failed;
    report.selected.reset();
    report.verification.reset();
    report.slots[0] = {"A", myrd::m3::SlotStatus::Rejected, 7,
                       "journal_commit", "corrupt", false};
    report.slots[1] = {"B", myrd::m3::SlotStatus::Empty, std::nullopt,
                       "slot_decode", "missing", false};
    report.error = myrd::m3::ErrorEvidence{
        "persistence", 12, 0, "inspect", "manifest_corrupt"};

    const auto json = myrd::m3::serialize_m3_report(report);
    REQUIRE(json.ok());
    REQUIRE(json.value().find("\"selected\": null") != std::string::npos);
    REQUIRE(json.value().find("journal_commit") != std::string::npos);
    REQUIRE(json.value().find("manifest_corrupt") != std::string::npos);
}

TEST_CASE(m3_inspect_is_read_only_and_preserves_both_slot_diagnostics) {
    using namespace myrd::persistence;
    InspectSandbox sandbox;
    const auto request = create_request(sandbox.root());
    auto created = create_generation_zero_image(myrd::OperationContext{}, request);
    REQUIRE(created.ok());
    created.value().backing_view.store.reset();

    const auto manifest_path = sandbox.root() / L"disk.rdm";
    const auto base_path = sandbox.root() / L"base-0000.vhdx";
    const auto journal_path = sandbox.root() / L"journal-e0001.rdj";
    const auto manifest_before = read_file(manifest_path);
    const auto base_before = read_file(base_path);
    const auto journal_before = read_file(journal_path);

    ImageInspectOptions options{};
    options.manifest_relative_name = "disk.rdm";
    options.recovery = request.recovery;
    const auto inspected = inspect_image(
        myrd::OperationContext{}, sandbox.root(), options);

    REQUIRE(inspected.ok());
    REQUIRE(inspected.value().selected.has_value());
    REQUIRE_EQ(inspected.value().selected->selected_slot, std::uint8_t{0});
    REQUIRE_EQ(inspected.value().selected->geometry.virtual_bytes,
               request.geometry.virtual_bytes);
    REQUIRE(inspected.value().diagnostics[0].selected);
    REQUIRE(!inspected.value().diagnostics[1].selected);
    const auto report = myrd::m3::make_m3_report(
        inspected.value(), report_context());
    REQUIRE(report.ok());
    const auto json = myrd::m3::serialize_m3_report(report.value());
    REQUIRE(json.ok());
    REQUIRE(json.value().find("11000000000000000000000000000000") !=
            std::string::npos);
    REQUIRE(json.value().find("base-0000.vhdx") == std::string::npos);
    REQUIRE_EQ(read_file(manifest_path), manifest_before);
    REQUIRE_EQ(read_file(base_path), base_before);
    REQUIRE_EQ(read_file(journal_path), journal_before);
}

TEST_CASE(m3_inspect_reports_corrupt_no_selection_without_repair) {
    using namespace myrd::persistence;
    InspectSandbox sandbox;
    const auto request = create_request(sandbox.root());
    auto created = create_generation_zero_image(myrd::OperationContext{}, request);
    REQUIRE(created.ok());
    created.value().backing_view.store.reset();

    const auto manifest_path = sandbox.root() / L"disk.rdm";
    {
        std::fstream file(manifest_path,
                          std::ios::binary | std::ios::in | std::ios::out);
        const char corrupt = '\x7f';
        file.seekp(0);
        file.write(&corrupt, 1);
    }
    const auto corrupt_before = read_file(manifest_path);

    ImageInspectOptions options{};
    options.manifest_relative_name = "disk.rdm";
    options.recovery = request.recovery;
    const auto inspected = inspect_image(
        myrd::OperationContext{}, sandbox.root(), options);

    REQUIRE(inspected.ok());
    REQUIRE(!inspected.value().selected.has_value());
    REQUIRE(inspected.value().rejection.has_value());
    REQUIRE_EQ(inspected.value().diagnostics[0].status,
               ManifestCandidateCheckStatus::Corrupt);
    REQUIRE_EQ(read_file(manifest_path), corrupt_before);
    const auto report = myrd::m3::make_m3_report(
        inspected.value(), report_context());
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().status, myrd::m3::ReportStatus::Failed);
    REQUIRE(!report.value().selected.has_value());
    REQUIRE(report.value().error.has_value());
}

TEST_CASE(m3_full_verify_streams_the_recovered_logical_view) {
    using namespace myrd::persistence;
    InspectSandbox sandbox;
    const auto request = create_request(sandbox.root());
    auto created = create_generation_zero_image(myrd::OperationContext{}, request);
    REQUIRE(created.ok());
    created.value().backing_view.store.reset();

    ImageInspectOptions options{};
    options.manifest_relative_name = "disk.rdm";
    options.recovery = request.recovery;
    const auto verified = verify_image(
        myrd::OperationContext{}, sandbox.root(),
        VerificationDepth::Full, options);

    REQUIRE(verified.ok());
    REQUIRE(verified.value().verified);
    REQUIRE_EQ(verified.value().candidate_blocks, std::uint64_t{0});
    REQUIRE_EQ(verified.value().data_blocks, std::uint64_t{0});
    REQUIRE_EQ(verified.value().zero_blocks, std::uint64_t{0});
    REQUIRE_EQ(verified.value().logical_sha256.size(), std::size_t{64});
    const auto report = myrd::m3::make_m3_report(
        verified.value(), report_context());
    REQUIRE(report.ok());
    REQUIRE_EQ(report.value().operation,
               myrd::m3::ReportOperation::VerifyFull);
    REQUIRE_EQ(report.value().status, myrd::m3::ReportStatus::Passed);
    REQUIRE(myrd::m3::serialize_m3_report(report.value()).ok());
}

TEST_CASE(m3_full_verify_streams_data_zero_discard_and_partial_tail) {
    using namespace myrd::persistence;
    InspectSandbox sandbox;
    auto request = create_request(sandbox.root());
    request.geometry.virtual_bytes = 2 * kM2BlockSize + 512;
    auto created = create_generation_zero_image(myrd::OperationContext{}, request);
    REQUIRE(created.ok());
    auto active = created.value().selected_manifest;
    created.value().backing_view.store.reset();
    static_cast<void>(publish_mixed_checkpoint(sandbox.root(), active));

    ImageInspectOptions options{};
    options.manifest_relative_name = "disk.rdm";
    options.recovery = request.recovery;
    const auto verified = verify_image(
        myrd::OperationContext{}, sandbox.root(),
        VerificationDepth::Full, options);

    REQUIRE(verified.ok());
    REQUIRE(verified.value().verified);
    REQUIRE(!verified.value().failure.has_value());
    REQUIRE_EQ(verified.value().candidate_blocks, std::uint64_t{3});
    REQUIRE_EQ(verified.value().data_blocks, std::uint64_t{1});
    REQUIRE_EQ(verified.value().zero_blocks, std::uint64_t{2});
    REQUIRE_EQ(verified.value().data_bytes_hashed, kM2BlockSize);
    REQUIRE_EQ(verified.value().logical_sha256,
        std::string{"82DEB95688E6213CAD8225D57292DBE5C188DB99C2EE77E84A64AF5FC3271910"});
}

TEST_CASE(m3_full_verify_payload_corruption_cannot_pass_via_fallback) {
    using namespace myrd::persistence;
    InspectSandbox sandbox;
    auto request = create_request(sandbox.root());
    request.geometry.virtual_bytes = 3 * kM2BlockSize;
    auto created = create_generation_zero_image(myrd::OperationContext{}, request);
    REQUIRE(created.ok());
    auto active = created.value().selected_manifest;
    created.value().backing_view.store.reset();
    const auto first_record = publish_mixed_checkpoint(sandbox.root(), active);
    {
        std::fstream journal(
            sandbox.root() / L"journal-e0001.rdj",
            std::ios::binary | std::ios::in | std::ios::out);
        journal.seekp(static_cast<std::streamoff>(
            first_record + kJournalAlignment));
        const char corrupt = '\x33';
        journal.write(&corrupt, 1);
        journal.flush();
    }
    ImageInspectOptions options{};
    options.manifest_relative_name = "disk.rdm";
    options.recovery = request.recovery;
    const auto verified = verify_image(
        myrd::OperationContext{}, sandbox.root(),
        VerificationDepth::Full, options);
    REQUIRE(verified.ok());
    REQUIRE(!verified.value().verified);
    REQUIRE(verified.value().inspection.selected.has_value());
    REQUIRE(verified.value().inspection.selected->fallback);
    REQUIRE(verified.value().failure.has_value());
}

TEST_CASE(m3_full_verify_refuses_corrupt_newer_generation_fallback) {
    using namespace myrd::persistence;
    InspectSandbox sandbox;
    const auto request = create_request(sandbox.root());
    auto created = create_generation_zero_image(myrd::OperationContext{}, request);
    REQUIRE(created.ok());
    auto newer = created.value().selected_manifest;
    created.value().backing_view.store.reset();
    newer.slot_index = 1;
    newer.generation.checkpoint_generation = 1;
    ++newer.committed_time_100ns;
    const auto encoded = encode_manifest_slot(newer);
    REQUIRE(encoded.ok());
    {
        std::fstream manifest(
            sandbox.root() / L"disk.rdm",
            std::ios::binary | std::ios::in | std::ios::out);
        manifest.seekp(static_cast<std::streamoff>(kManifestSlotBytes));
        manifest.write(reinterpret_cast<const char*>(encoded.value().data()),
                       static_cast<std::streamsize>(encoded.value().size()));
        manifest.flush();
    }

    ImageInspectOptions options{};
    options.manifest_relative_name = "disk.rdm";
    options.recovery = request.recovery;
    const auto verified = verify_image(
        myrd::OperationContext{}, sandbox.root(),
        VerificationDepth::Full, options);

    REQUIRE(verified.ok());
    REQUIRE(!verified.value().verified);
    REQUIRE(verified.value().failure.has_value());
    REQUIRE(verified.value().inspection.selected.has_value());
    REQUIRE(verified.value().inspection.selected->fallback);
    REQUIRE_EQ(verified.value().failure->code,
               static_cast<std::uint32_t>(
                   PersistenceErrorCode::VerificationFailed));
}

TEST_CASE(m3_inspect_rejects_invalid_caller_limits_as_usage_not_corruption) {
    myrd::persistence::ImageInspectOptions options{};
    options.manifest_relative_name = "disk.rdm";
    const auto inspected = myrd::persistence::inspect_image(
        myrd::OperationContext{}, std::filesystem::temp_directory_path(),
        options);
    REQUIRE(!inspected.ok());
    REQUIRE_EQ(inspected.error().code, static_cast<std::uint32_t>(
        myrd::persistence::PersistenceErrorCode::InvalidFormat));
}

TEST_CASE(m3_report_adapter_rejects_unknown_persistence_verify_depth) {
    myrd::persistence::ImageVerificationReport verification{};
    verification.depth = static_cast<
        myrd::persistence::VerificationDepth>(255);
    REQUIRE(!myrd::m3::make_m3_report(
        verification, report_context()).ok());
}

TEST_CASE(m3_evidence_publish_is_exact_atomic_and_no_overwrite) {
    using namespace myrd::m3;
    InspectSandbox sandbox;
    const auto final_path = sandbox.root() / L"evidence.json";
    const std::string json = "{\"schema_version\":1}\n";

    const auto published = publish_m3_evidence(
        myrd::OperationContext{}, final_path, json, {});
    REQUIRE(published.published);
    REQUIRE(!published.error.has_value());
    REQUIRE_EQ(read_file(final_path),
               std::vector<char>(json.begin(), json.end()));

    const auto collision = publish_m3_evidence(
        myrd::OperationContext{}, final_path, "replacement", {});
    REQUIRE(!collision.published);
    REQUIRE(collision.error.has_value());
    REQUIRE(collision.retained_temporary_path.has_value());
    REQUIRE_EQ(collision.retained_temporary_path->parent_path(),
               final_path.parent_path());
    REQUIRE_EQ(read_file(final_path),
               std::vector<char>(json.begin(), json.end()));
}

TEST_CASE(m3_evidence_publish_faults_never_expose_partial_final_json) {
    using namespace myrd::m3;
    for (const auto phase : {
             EvidencePublishPhase::AfterTemporaryCreate,
             EvidencePublishPhase::AfterExactWrite,
             EvidencePublishPhase::AfterFlush,
             EvidencePublishPhase::BeforeRename}) {
        InspectSandbox sandbox;
        const auto final_path = sandbox.root() / L"evidence.json";
        EvidencePublishOptions options{};
        options.phase_hook = [phase](EvidencePublishPhase current) {
            if (current == phase) {
                return myrd::Result<void>::failure({
                    myrd::ErrorDomain::evidence, 3705,
                    L"m3_evidence_test", L"injected publish fault"});
            }
            return myrd::Result<void>::success();
        };

        const auto result = publish_m3_evidence(
            myrd::OperationContext{}, final_path, "{\"ok\":true}\n", options);
        REQUIRE(!result.published);
        REQUIRE(result.error.has_value());
        REQUIRE(result.retained_temporary_path.has_value());
        REQUIRE(std::filesystem::exists(*result.retained_temporary_path));
        REQUIRE(!std::filesystem::exists(final_path));
        REQUIRE_EQ(result.retained_temporary_path->parent_path(),
                   final_path.parent_path());
    }
}

TEST_CASE(m3_evidence_publish_pins_exact_temp_object_through_rename) {
    using namespace myrd::m3;
    InspectSandbox sandbox;
    const auto final_path = sandbox.root() / L"evidence.json";
    bool sharing_denied{};
    EvidencePublishOptions options{};
    options.phase_hook = [&](EvidencePublishPhase phase) {
        if (phase == EvidencePublishPhase::BeforeRename) {
            for (const auto& entry :
                 std::filesystem::directory_iterator(sandbox.root())) {
                if (entry.path().filename().wstring().find(L".tmp-") ==
                    std::wstring::npos) {
                    continue;
                }
                std::ofstream competing(
                    entry.path(), std::ios::binary | std::ios::app);
                sharing_denied = !competing.is_open();
            }
        }
        return myrd::Result<void>::success();
    };
    const auto result = publish_m3_evidence(
        myrd::OperationContext{}, final_path, "{\"ok\":true}\n", options);
    REQUIRE(result.published);
    REQUIRE(sharing_denied);
    REQUIRE(std::filesystem::exists(final_path));
}

TEST_CASE(m3_evidence_publish_rejects_ambiguous_relative_target) {
    const auto result = myrd::m3::publish_m3_evidence(
        myrd::OperationContext{}, L"evidence.json", "{}\n", {});
    REQUIRE(!result.published);
    REQUIRE(result.error.has_value());
    REQUIRE(!result.retained_temporary_path.has_value());
}
