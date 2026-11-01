#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#include "test_framework.h"
#include "test_paths.h"

#include "src/persistence/commit_resolver.h"
#include "src/persistence/file_io.h"
#include "src/persistence/journal_writer.h"
#include "src/persistence/manifest_format.h"
#include "src/persistence/recovery_engine.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/vhdx_writer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::persistence {
namespace {

struct Scenario final {
    std::string id;
    std::string event;
    std::string allowed_recovery;
    std::string survivor_disposition;
    std::string survivor_phase;
    bool tail_garbage_allowed{};
};

struct RegistryPhase final {
    std::string id;
    bool short_write{};
    bool torn{};
    bool kill{};
    std::string error_disposition;
    std::string kill_recovery;
};

std::string load_text(const char* path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.good()) {
        throw std::runtime_error("required Task33 fixture is missing");
    }
    return {std::istreambuf_iterator<char>{input},
            std::istreambuf_iterator<char>{}};
}

std::string load_matrix_json() {
    return load_text(MYRD_TEST_PATH(MYRD_M3_CHECKPOINT_CRASH_MATRIX_PATH));
}

std::vector<Scenario> parse_matrix(const std::string& json) {
    const std::regex object{
        R"json(\{\s*"id"\s*:\s*"([^"]+)"\s*,\s*"event"\s*:\s*"([^"]+)"\s*,\s*"allowed_recovery"\s*:\s*"([^"]+)"\s*,\s*"survivor_disposition"\s*:\s*"([^"]+)"\s*,\s*"survivor_phase"\s*:\s*"([^"]+)"\s*,\s*"tail_garbage_allowed"\s*:\s*(true|false)\s*\})json"};
    std::vector<Scenario> scenarios;
    for (auto it = std::sregex_iterator(json.begin(), json.end(), object);
         it != std::sregex_iterator{}; ++it) {
        scenarios.push_back({
            (*it)[1].str(), (*it)[2].str(), (*it)[3].str(),
            (*it)[4].str(), (*it)[5].str(), (*it)[6].str() == "true"});
    }
    return scenarios;
}

std::vector<RegistryPhase> parse_registry(const std::string& json) {
    const std::regex object{
        R"json(\{\s*"id"\s*:\s*"([^"]+)"\s*,\s*"boundary"\s*:\s*"[^"]+"\s*,\s*"short"\s*:\s*(true|false)\s*,\s*"torn"\s*:\s*(true|false)\s*,\s*"kill"\s*:\s*(true|false)\s*,\s*"gate"\s*:\s*"[^"]+"\s*,\s*"state_disposition"\s*:\s*"[^"]+"\s*,\s*"error_disposition"\s*:\s*"([^"]+)"\s*,\s*"kill_recovery"\s*:\s*"([^"]+)"\s*\})json"};
    std::vector<RegistryPhase> phases;
    for (auto it = std::sregex_iterator(json.begin(), json.end(), object);
         it != std::sregex_iterator{}; ++it) {
        phases.push_back({
            (*it)[1].str(), (*it)[2].str() == "true",
            (*it)[3].str() == "true", (*it)[4].str() == "true",
            (*it)[5].str(), (*it)[6].str()});
    }
    return phases;
}

bool starts_with(const std::string_view value,
                 const std::string_view prefix) {
    return value.size() >= prefix.size() &&
           value.substr(0, prefix.size()) == prefix;
}

std::size_t count_occurrences(const std::string_view text,
                              const std::string_view needle) {
    std::size_t count{};
    std::size_t offset{};
    while ((offset = text.find(needle, offset)) != std::string_view::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

int stage_of(const std::string_view id) {
    if (starts_with(id, "journal.append.data.")) return 0;
    if (starts_with(id, "journal.append.zero.")) return 1;
    if (starts_with(id, "journal.append.discard.")) return 2;
    if (starts_with(id, "journal.append.index_node.")) return 3;
    if (starts_with(id, "journal.commit.append.")) return 4;
    if (starts_with(id, "journal.flush.")) return 5;
    if (starts_with(id, "journal.readback.")) return 6;
    if (starts_with(id, "manifest.slot_write.")) return 7;
    if (starts_with(id, "manifest.flush.")) return 8;
    if (starts_with(id, "manifest.readback.")) return 9;
    if (starts_with(id, "backing.view_install.")) return 10;
    if (starts_with(id, "backing.ack.each.")) return 11;
    if (starts_with(id, "reconciliation.")) return 12;
    return -1;
}

std::string registry_allowed_recovery(const RegistryPhase& phase) {
    if (phase.kill_recovery == "OldCompleteGenerationOnly") return "old";
    if (phase.kill_recovery == "OldOrNewCompleteGeneration") {
        return "old_or_new";
    }
    if (phase.kill_recovery == "NewCompleteGenerationOnly") return "new";
    if (phase.kill_recovery == "NotApplicable") return "not_applicable";
    return "invalid";
}

std::string normalized_disposition(const std::string_view value) {
    if (value == "NotCommitted") return "not_committed";
    if (value == "CommitIndeterminate") return "commit_indeterminate";
    if (value == "CommittedNeedsReconciliation") {
        return "committed_needs_reconciliation";
    }
    if (value == "Committed") return "committed";
    return "not_applicable";
}

std::string expected_survivor_disposition(
    const Scenario& scenario, const RegistryPhase& phase) {
    if (phase.error_disposition != "NotApplicable") {
        return normalized_disposition(phase.error_disposition);
    }
    if (scenario.id == "reconciliation.after") return "committed";
    const auto stage = stage_of(scenario.id);
    if (stage < 7 || scenario.id == "manifest.slot_write.before") {
        return "not_committed";
    }
    if (stage == 7 ||
        stage == 8 && scenario.id != "manifest.flush.after") {
        return "commit_indeterminate";
    }
    return "committed_needs_reconciliation";
}

std::string expected_survivor_phase(const std::string_view disposition) {
    if (disposition == "not_committed" || disposition == "committed") {
        return "ready";
    }
    if (disposition == "commit_indeterminate") {
        return "resolving_commit";
    }
    return "reconciling";
}

bool matrix_matches_transaction_rules(
    const std::vector<Scenario>& scenarios,
    const std::vector<RegistryPhase>& registry) {
    std::map<std::string, RegistryPhase> phases;
    std::size_t relevant_phase_count{};
    for (const auto& phase : registry) {
        if (!phases.emplace(phase.id, phase).second) return false;
        if (stage_of(phase.id) >= 0) ++relevant_phase_count;
    }
    if (scenarios.size() != relevant_phase_count) return false;
    std::set<std::string> ids;
    int previous_stage = -1;
    std::set<int> observed_stages;
    for (const auto& scenario : scenarios) {
        if (scenario.id.empty() || scenario.event.empty() ||
            !ids.insert(scenario.id).second) {
            return false;
        }
        const auto stage = stage_of(scenario.id);
        if (stage < previous_stage || stage < 0) return false;
        previous_stage = stage;
        observed_stages.insert(stage);
        const auto found = phases.find(scenario.id);
        if (found == phases.end()) return false;
        const auto expected_disposition =
            expected_survivor_disposition(scenario, found->second);
        const bool post_commit =
            scenario.id == "manifest.flush.after" || stage > 8;
        if (scenario.allowed_recovery !=
                registry_allowed_recovery(found->second) ||
            scenario.survivor_disposition != expected_disposition ||
            scenario.survivor_phase !=
                expected_survivor_phase(expected_disposition) ||
            post_commit && scenario.tail_garbage_allowed) {
            return false;
        }
    }
    return observed_stages.size() == 13;
}

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::byte kBaseByte{0x5b};
constexpr std::byte kOldDataByte{0x6a};
constexpr std::byte kNewDataByte{0x7b};

void write_file_bytes(const std::filesystem::path& path,
                      const std::span<const std::byte> bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.good()) throw std::runtime_error("cannot create crash clone");
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    if (!output.good()) throw std::runtime_error("cannot write crash clone");
}

std::vector<std::byte> read_file_bytes(
    const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) throw std::runtime_error("cannot read crash fixture");
    const std::vector<char> chars{std::istreambuf_iterator<char>{input},
                                  std::istreambuf_iterator<char>{}};
    std::vector<std::byte> bytes(chars.size());
    std::transform(chars.begin(), chars.end(), bytes.begin(),
                   [](const char value) {
                       return static_cast<std::byte>(
                           static_cast<unsigned char>(value));
                   });
    return bytes;
}

class CrashPayloadSource final : public IVhdxPayloadSource {
public:
    std::map<std::uint64_t, std::vector<std::byte>> payloads;

    Result<void> read_payload(
        const OperationContext&, const std::uint64_t index,
        const std::span<std::byte> destination) const override {
        const auto found = payloads.find(index);
        if (found == payloads.end() ||
            found->second.size() != destination.size()) {
            return Result<void>::failure({
                ErrorDomain::internal, 0x6333,
                L"CrashPayloadSource::read_payload",
                L"fixture payload missing"});
        }
        std::copy(found->second.begin(), found->second.end(),
                  destination.begin());
        return Result<void>::success();
    }
};

class CrashBundle final {
public:
    CrashBundle() {
        static std::uint64_t sequence{};
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-task33-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(sequence++));
        std::filesystem::create_directories(root_);
        manifest_path_ = root_ / L"disk.rdm";
        base_path_ = root_ / L"base.vhdx";
        journal_path_ = root_ / L"journal.rdj";
        image_uuid_.bytes[0] = std::byte{0x31};
        base_identity_.file_identity.file_write_guid[0] = std::byte{0x41};
        base_identity_.file_identity.data_write_guid[0] = std::byte{0x42};
        base_identity_.virtual_disk_id[0] = std::byte{0x43};
        build_base();
        build_old_journal();
        old_slot_ = make_slot(0, old_checkpoint_);
        publish(old_slot_, std::nullopt);
        old_journal_ = read_file_bytes(journal_path_);
        old_manifest_ = read_file_bytes(manifest_path_);
        append_new_checkpoint();
        const auto candidate = prepare_manifest_candidate(
            old_slot_, new_checkpoint_, 300);
        REQUIRE(candidate.ok());
        new_slot_ = candidate.value().slot;
        publish(old_slot_, new_slot_);
        final_journal_ = read_file_bytes(journal_path_);
        final_manifest_ = read_file_bytes(manifest_path_);
    }

    ~CrashBundle() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    CrashBundle(const CrashBundle&) = delete;
    CrashBundle& operator=(const CrashBundle&) = delete;

    void install_clone(const Scenario& scenario) const {
        auto journal = journal_clone(scenario);
        auto manifest = manifest_clone(scenario);
        if (scenario.tail_garbage_allowed) {
            journal.insert(journal.end(), kJournalAlignment,
                           std::byte{0xcd});
        }
        write_file_bytes(journal_path_, journal);
        write_file_bytes(manifest_path_, manifest);
    }

    std::uint64_t journal_target_length(const Scenario& scenario) const {
        const auto regions = write_regions();
        const auto target = target_region(scenario.id, regions);
        return target.has_value() ? regions[*target].length : 0;
    }

    void install_journal_short_clone(
        const Scenario& scenario, const std::uint64_t short_length) const {
        auto journal = journal_clone(scenario);
        const auto regions = write_regions();
        const auto target = target_region(scenario.id, regions);
        REQUIRE(target.has_value());
        REQUIRE(short_length > 0);
        REQUIRE(short_length < regions[*target].length);
        copy_region(journal, regions[*target], short_length);
        journal.insert(journal.end(), kJournalAlignment, std::byte{0xce});
        write_file_bytes(journal_path_, journal);
        write_file_bytes(manifest_path_, old_manifest_);
    }

    void install_manifest_torn_clone(
        const std::size_t torn_length) const {
        REQUIRE(torn_length > 0);
        REQUIRE(torn_length < kManifestSlotBytes);
        REQUIRE(torn_length % 512 == 0);
        auto manifest = old_manifest_;
        std::copy_n(final_manifest_.begin() + kManifestSlotBytes,
                    static_cast<std::ptrdiff_t>(torn_length),
                    manifest.begin() + kManifestSlotBytes);
        write_file_bytes(journal_path_, final_journal_);
        write_file_bytes(manifest_path_, manifest);
    }

    RecoveryOutcome recover() const {
        RecoveryOptions options{};
        options.limits = {
            64 * kMiB, 64 * kMiB, 8192,
            64 * kIndexNodeBytes, 64,
            64 * kVhdxBatPageBytes, 64,
            2 * kMiB, 2 * kMiB};
        return recover_complete_image(
            OperationContext{}, root_, "disk.rdm", options);
    }

    CheckpointGeneration old_generation() const noexcept {
        return old_checkpoint_.generation;
    }

    CheckpointGeneration new_generation() const noexcept {
        return new_checkpoint_.generation;
    }

    std::uint64_t logical_block_count() const noexcept {
        return geometry_.virtual_bytes / kM2BlockSize;
    }

private:
    ManifestSlot make_slot(const std::uint8_t slot_index,
                           const PreparedCheckpoint& checkpoint) const {
        ManifestSlot slot{};
        slot.slot_index = slot_index;
        slot.image_uuid = image_uuid_;
        slot.generation.checkpoint_generation = checkpoint.generation;
        slot.generation.base_generation = 0;
        slot.generation.journal.epoch = journal_header_.epoch;
        slot.generation.journal.committed_length =
            checkpoint.committed_length;
        slot.generation.journal.commit = checkpoint.commit;
        slot.generation.journal.root = checkpoint.root;
        slot.geometry = geometry_;
        slot.base_relative_name = "base.vhdx";
        slot.base_identity = base_identity_.file_identity;
        slot.base_expected_length = static_cast<std::uint64_t>(
            std::filesystem::file_size(base_path_));
        slot.journal_relative_name = "journal.rdj";
        slot.journal_identity = journal_header_.journal_identity;
        slot.created_time_100ns = 100;
        slot.committed_time_100ns = 100;
        return slot;
    }

    void publish(const std::optional<ManifestSlot>& a,
                 const std::optional<ManifestSlot>& b) const {
        std::array<std::byte, kManifestFileBytes> bytes{};
        if (a.has_value()) {
            const auto encoded = encode_manifest_slot(*a);
            REQUIRE(encoded.ok());
            std::copy(encoded.value().begin(), encoded.value().end(),
                      bytes.begin());
        }
        if (b.has_value()) {
            const auto encoded = encode_manifest_slot(*b);
            REQUIRE(encoded.ok());
            std::copy(encoded.value().begin(), encoded.value().end(),
                      bytes.begin() + kManifestSlotBytes);
        }
        write_file_bytes(manifest_path_, bytes);
    }

    void build_base() {
        CrashPayloadSource source;
        source.payloads.emplace(
            0, std::vector<std::byte>(kVhdxDataBlockBytes, kBaseByte));
        const std::array<std::uint64_t, 1> candidates{0};
        auto plan = plan_sparse_vhdx(
            OperationContext{},
            {geometry_.virtual_bytes, geometry_.logical_sector_size},
            base_identity_, candidates, source,
            {8, 8, kVhdxDataBlockBytes});
        REQUIRE(plan.ok());
        auto opened = open_windows_persistence_file(
            base_path_, FileOpenMode::CreateNewReadWrite);
        REQUIRE(opened.ok());
        auto written = write_sparse_vhdx_file(
            OperationContext{}, plan.value(), source, opened.move_value());
        REQUIRE(written.ok());
        auto file = written.value().release_file();
        REQUIRE(file->close().ok());
    }

    JournalWriterOptions writer_options() {
        JournalWriterOptions options{};
        options.index_builder = {
            {64 * kIndexNodeBytes, 64}, 4096,
            4096 * kIndexNodeBytes};
        options.max_checkpoint_records = 8192;
        options.max_checkpoint_bytes = 64 * kMiB;
#if defined(MYRD_ENABLE_TEST_SEAMS)
        options.phase_hook = [this](const JournalWriterPhaseEvent& event) {
            new_phases_.push_back(event);
            return Result<void>::success();
        };
#endif
        return options;
    }

    void build_old_journal() {
        journal_header_.image_uuid = image_uuid_;
        journal_header_.journal_identity.journal_uuid[0] =
            std::byte{0x51};
        journal_header_.epoch = 1;
        journal_header_.geometry = geometry_;
        journal_header_.geometry_digest =
            compute_geometry_digest(geometry_).value();
        journal_header_.base_identity_digest =
            compute_base_identity_digest(
                base_identity_.file_identity).value();
        journal_header_.predecessor_digest = compute_genesis_digest(
            image_uuid_, journal_header_.geometry_digest,
            journal_header_.base_identity_digest).value();

        auto opened = open_windows_persistence_file(
            journal_path_, FileOpenMode::CreateNewReadWrite);
        REQUIRE(opened.ok());
        auto file = std::shared_ptr<IPersistenceFile>(opened.move_value());
        const auto header = encode_journal_header(journal_header_);
        REQUIRE(header.ok());
        REQUIRE(file->write_exact(0, header.value()).ok());

        std::array<std::byte, kM2BlockSize> payload{};
        payload.fill(kOldDataByte);
        const std::array<JournalCheckpointBlockInput, 2> blocks{{
            {JournalRecordType::Data, 1, 7, 0, 3, payload},
            {JournalRecordType::Zero, 2, 8, 0, 3, {}}}};
        JournalCheckpointRequest request{};
        request.file = file;
        request.file_identity.volume_serial_number = 1;
        request.file_identity.file_id[0] = std::byte{1};
        request.file_identity.size = kJournalAlignment;
        request.file_identity.hard_link_count = 1;
        request.header = journal_header_;
        request.previous_digest = journal_header_.predecessor_digest;
        request.committed_length = kJournalAlignment;
        request.operation_id = 3301;
        request.blocks = blocks;
        auto options = writer_options();
#if defined(MYRD_ENABLE_TEST_SEAMS)
        options.phase_hook = {};
#endif
        auto writer = JournalWriter::create(std::move(options));
        REQUIRE(writer.ok());
        auto prepared = writer.value()->prepare(OperationContext{}, request);
        REQUIRE(prepared.ok());
        old_checkpoint_ = prepared.move_value();
        REQUIRE(file->close().ok());
    }

    void append_new_checkpoint() {
        auto opened = open_windows_persistence_file(
            journal_path_, FileOpenMode::ReadWriteExisting);
        REQUIRE(opened.ok());
        auto file = std::shared_ptr<IPersistenceFile>(opened.move_value());
        std::array<std::byte, kM2BlockSize> payload{};
        payload.fill(kNewDataByte);
        const std::array<JournalCheckpointBlockInput, 3> blocks{{
            {JournalRecordType::Data, 1, 9,
             old_checkpoint_.generation, 4, payload},
            {JournalRecordType::Zero, 2, 10,
             old_checkpoint_.generation, 4, {}},
            {JournalRecordType::Discard, 3, 11,
             old_checkpoint_.generation, 4, {}}}};
        JournalCheckpointRequest request{};
        request.file = file;
        request.file_identity.volume_serial_number = 1;
        request.file_identity.file_id[0] = std::byte{1};
        request.file_identity.size = old_checkpoint_.committed_length;
        request.file_identity.hard_link_count = 1;
        request.header = journal_header_;
        request.previous_generation = old_checkpoint_.generation;
        request.previous_digest = old_checkpoint_.chain_digest;
        request.committed_length = old_checkpoint_.committed_length;
        request.root = old_checkpoint_.root;
        request.operation_id = 3302;
        request.blocks = blocks;
        auto writer = JournalWriter::create(writer_options());
        REQUIRE(writer.ok());
        auto prepared = writer.value()->prepare(OperationContext{}, request);
        REQUIRE(prepared.ok());
        new_checkpoint_ = prepared.move_value();
        REQUIRE(file->close().ok());
    }

    std::vector<JournalWriterPhaseEvent> write_regions() const {
        std::vector<JournalWriterPhaseEvent> regions;
        for (const auto& event : new_phases_) {
            if (event.phase == JournalWriterPhase::BeforeBlockWrite ||
                event.phase == JournalWriterPhase::BeforeIndexWrite ||
                event.phase == JournalWriterPhase::BeforeCommitWrite) {
                regions.push_back(event);
            }
        }
        return regions;
    }

    std::optional<std::size_t> target_region(
        const std::string_view id,
        const std::vector<JournalWriterPhaseEvent>& regions) const {
        if (starts_with(id, "journal.append.data.")) return 0;
        if (starts_with(id, "journal.append.zero.")) return 1;
        if (starts_with(id, "journal.append.discard.")) return 2;
        if (starts_with(id, "journal.append.index_node.")) {
            for (std::size_t index = 3; index < regions.size(); ++index) {
                if (regions[index].phase ==
                    JournalWriterPhase::BeforeIndexWrite) {
                    return index;
                }
            }
        }
        if (starts_with(id, "journal.commit.append.")) {
            for (std::size_t index = 3; index < regions.size(); ++index) {
                if (regions[index].phase ==
                    JournalWriterPhase::BeforeCommitWrite) {
                    return index;
                }
            }
        }
        return std::nullopt;
    }

    void copy_region(std::vector<std::byte>& destination,
                     const JournalWriterPhaseEvent& region,
                     const std::uint64_t length) const {
        REQUIRE(region.offset <= final_journal_.size());
        REQUIRE(length <= region.length);
        REQUIRE(length <= final_journal_.size() - region.offset);
        std::copy_n(final_journal_.begin() +
                        static_cast<std::ptrdiff_t>(region.offset),
                    static_cast<std::ptrdiff_t>(length),
                    destination.begin() +
                        static_cast<std::ptrdiff_t>(region.offset));
    }

    std::vector<std::byte> journal_clone(
        const Scenario& scenario) const {
        if (stage_of(scenario.id) >= 7) return final_journal_;
        auto clone = old_journal_;
        clone.resize(final_journal_.size());
        const auto regions = write_regions();
        const auto target = target_region(scenario.id, regions);
        if (!target.has_value()) {
            for (const auto& region : regions) {
                copy_region(clone, region, region.length);
            }
            return clone;
        }
        for (std::size_t index = 0; index < *target; ++index) {
            copy_region(clone, regions[index], regions[index].length);
        }
        if (scenario.id.ends_with(".short")) {
            copy_region(clone, regions[*target],
                        std::max<std::uint64_t>(1,
                            regions[*target].length / 2));
        } else if (scenario.id.ends_with(".after")) {
            copy_region(clone, regions[*target], regions[*target].length);
        }
        return clone;
    }

    std::vector<std::byte> manifest_clone(
        const Scenario& scenario) const {
        auto clone = old_manifest_;
        const auto stage = stage_of(scenario.id);
        if (stage < 7 || scenario.id == "manifest.slot_write.before") {
            return clone;
        }
        std::size_t length = kManifestSlotBytes;
        if (scenario.id == "manifest.slot_write.short") {
            length = kManifestSlotBytes / 2;
        } else if (scenario.id == "manifest.slot_write.torn") {
            length = 512;
        }
        std::copy_n(final_manifest_.begin() + kManifestSlotBytes,
                    static_cast<std::ptrdiff_t>(length),
                    clone.begin() + kManifestSlotBytes);
        return clone;
    }

    std::filesystem::path root_;
    std::filesystem::path manifest_path_;
    std::filesystem::path base_path_;
    std::filesystem::path journal_path_;
    ImageGeometry geometry_{8 * kMiB, 512, 4096};
    ImageUuid image_uuid_{};
    VhdxWriterIdentity base_identity_{};
    JournalHeader journal_header_{};
    PreparedCheckpoint old_checkpoint_{};
    PreparedCheckpoint new_checkpoint_{};
    ManifestSlot old_slot_{};
    ManifestSlot new_slot_{};
    std::vector<JournalWriterPhaseEvent> new_phases_{};
    std::vector<std::byte> old_journal_{};
    std::vector<std::byte> final_journal_{};
    std::vector<std::byte> old_manifest_{};
    std::vector<std::byte> final_manifest_{};
};

bool generation_is_allowed(const Scenario& scenario,
                           const CheckpointGeneration recovered,
                           const CrashBundle& bundle) {
    const bool old = recovered == bundle.old_generation();
    const bool next = recovered == bundle.new_generation();
    if (scenario.allowed_recovery == "old") return old;
    if (scenario.allowed_recovery == "new") return next;
    return scenario.allowed_recovery == "old_or_new" && (old || next);
}

std::byte expected_byte(const CheckpointGeneration generation,
                        const CheckpointGeneration old_generation,
                        const std::uint64_t block) {
    const bool next = generation != old_generation;
    if (block == 1) return next ? kNewDataByte : kOldDataByte;
    if (block == 2 || next && block == 3) return std::byte{0};
    return block < (2 * kMiB / kM2BlockSize)
        ? kBaseByte
        : std::byte{0};
}

void require_complete_map(const RecoveredImage& image,
                          const CrashBundle& bundle) {
    for (std::uint64_t block = 0; block < bundle.logical_block_count();
         ++block) {
        std::array<std::byte, kM2BlockSize> destination{};
        destination.fill(std::byte{0xa5});
        const auto loaded = image.backing_view.store->load_block(
            OperationContext{}, block, destination);
        REQUIRE(loaded.ok());
        const auto expected = expected_byte(
            image.checkpoint_generation, bundle.old_generation(), block);
        if (expected == std::byte{0}) {
            REQUIRE_EQ(loaded.value().kind, BlockLoadKind::AllZero);
            REQUIRE(std::all_of(
                destination.begin(), destination.end(),
                [](const std::byte value) {
                    return value == std::byte{0xa5};
                }));
        } else {
            REQUIRE_EQ(loaded.value().kind, BlockLoadKind::Data);
            REQUIRE(std::all_of(
                destination.begin(), destination.end(),
                [expected](const std::byte value) {
                    return value == expected;
                }));
        }
    }
}

using ModelMap = std::array<std::byte, 8>;

constexpr std::size_t kModelRecordBytes = 16;
constexpr std::size_t kModelRecordCount = 3;
constexpr std::size_t kModelIndexOffset =
    kModelRecordBytes * kModelRecordCount;
constexpr std::size_t kModelCommitOffset =
    kModelIndexOffset + kModelRecordBytes;
constexpr std::size_t kModelJournalBytes =
    kModelCommitOffset + kModelRecordBytes;
constexpr std::size_t kModelManifestBytes = 32;

std::uint64_t independent_hash(const std::span<const std::byte> bytes) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        value ^= std::to_integer<std::uint8_t>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

void model_put_u64(const std::span<std::byte> bytes,
                   const std::size_t offset,
                   const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        bytes[offset + index] = static_cast<std::byte>(
            (value >> (index * 8U)) & 0xffU);
    }
}

std::uint64_t model_get_u64(const std::span<const std::byte> bytes,
                            const std::size_t offset) {
    std::uint64_t value{};
    for (std::size_t index = 0; index < 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

ModelMap old_model_map() {
    return {std::byte{0x10}, std::byte{0x20}, std::byte{0x30},
            std::byte{0x40}, std::byte{0x50}, std::byte{0x60},
            std::byte{0x70}, std::byte{0x80}};
}

ModelMap new_model_map() {
    auto map = old_model_map();
    map[1] = std::byte{0xa1};
    map[2] = std::byte{0};
    map[3] = std::byte{0};
    return map;
}

std::array<std::byte, kModelJournalBytes> complete_model_journal() {
    std::array<std::byte, kModelJournalBytes> bytes{};
    const auto put_record = [&](const std::size_t ordinal,
                                const char kind,
                                const std::uint8_t block,
                                const std::uint8_t value) {
        const auto offset = ordinal * kModelRecordBytes;
        bytes[offset] = std::byte{'R'};
        bytes[offset + 1] = static_cast<std::byte>(kind);
        bytes[offset + 2] = static_cast<std::byte>(block);
        bytes[offset + 3] = static_cast<std::byte>(value);
        model_put_u64(bytes, offset + 8,
                      independent_hash(std::span<const std::byte>{bytes}
                          .subspan(offset, 8)));
    };
    put_record(0, 'D', 1, 0xa1);
    put_record(1, 'Z', 2, 0);
    put_record(2, 'X', 3, 0);
    bytes[kModelIndexOffset] = std::byte{'I'};
    model_put_u64(bytes, kModelIndexOffset + 8,
                  independent_hash(std::span<const std::byte>{bytes}
                      .first(kModelIndexOffset)));
    bytes[kModelCommitOffset] = std::byte{'C'};
    model_put_u64(bytes, kModelCommitOffset + 8,
                  independent_hash(std::span<const std::byte>{bytes}
                      .first(kModelCommitOffset)));
    return bytes;
}

std::array<std::byte, kModelManifestBytes> complete_model_manifest(
    const std::span<const std::byte> journal) {
    std::array<std::byte, kModelManifestBytes> bytes{};
    bytes[0] = std::byte{'N'};
    bytes[1] = std::byte{2};
    model_put_u64(bytes, 8,
                  independent_hash(journal.first(kModelJournalBytes)));
    model_put_u64(bytes, 16,
                  independent_hash(
                      std::span<const std::byte>{bytes}.first(16)));
    return bytes;
}

bool validate_model_journal(const std::span<const std::byte> bytes,
                            ModelMap& map) {
    if (bytes.size() < kModelJournalBytes) return false;
    map = old_model_map();
    constexpr std::array<char, 3> kinds{'D', 'Z', 'X'};
    constexpr std::array<std::uint8_t, 3> blocks{1, 2, 3};
    for (std::size_t ordinal = 0; ordinal < kModelRecordCount;
         ++ordinal) {
        const auto offset = ordinal * kModelRecordBytes;
        if (bytes[offset] != std::byte{'R'} ||
            bytes[offset + 1] != static_cast<std::byte>(kinds[ordinal]) ||
            bytes[offset + 2] != static_cast<std::byte>(blocks[ordinal]) ||
            model_get_u64(bytes, offset + 8) !=
                independent_hash(bytes.subspan(offset, 8))) {
            return false;
        }
        if (kinds[ordinal] == 'D') {
            map[blocks[ordinal]] = bytes[offset + 3];
        } else {
            map[blocks[ordinal]] = std::byte{0};
        }
    }
    if (bytes[kModelIndexOffset] != std::byte{'I'} ||
        model_get_u64(bytes, kModelIndexOffset + 8) !=
            independent_hash(bytes.first(kModelIndexOffset)) ||
        bytes[kModelCommitOffset] != std::byte{'C'} ||
        model_get_u64(bytes, kModelCommitOffset + 8) !=
            independent_hash(bytes.first(kModelCommitOffset))) {
        return false;
    }
    return true;
}

enum class ModelGeneration { Old, New };

struct ModelRecovery final {
    ModelGeneration generation{ModelGeneration::Old};
    ModelMap map{old_model_map()};
};

struct ModelClone final {
    std::vector<std::byte> journal;
    std::vector<std::byte> manifest;
};

ModelRecovery recover_model_clone(const ModelClone& clone) {
    ModelRecovery result{};
    if (clone.manifest.size() < kModelManifestBytes ||
        clone.manifest[0] != std::byte{'N'} ||
        clone.manifest[1] != std::byte{2} ||
        model_get_u64(clone.manifest, 16) !=
            independent_hash(
                std::span<const std::byte>{clone.manifest}.first(16))) {
        return result;
    }
    ModelMap next{};
    if (!validate_model_journal(clone.journal, next) ||
        model_get_u64(clone.manifest, 8) !=
            independent_hash(
                std::span<const std::byte>{clone.journal}
                    .first(kModelJournalBytes))) {
        return result;
    }
    result.generation = ModelGeneration::New;
    result.map = next;
    return result;
}

enum class ModelCut : std::uint8_t {
    DataWrite,
    ZeroWrite,
    DiscardWrite,
    IndexWrite,
    CommitWrite,
    JournalFlush,
    JournalReadback,
    ManifestAttempt,
    ManifestFlush,
    Install,
    Ack,
};

struct ModelSchedule final {
    enum class Profile : std::uint8_t { Quick, Ci, Manual };

    std::uint64_t seed{};
    ModelCut cut{ModelCut::DataWrite};
    std::uint8_t journal_regions{};
    std::uint16_t manifest_bytes{};
    bool tail_garbage{};
    bool ack_before_manifest{};
    Profile profile{Profile::Ci};
};

class ModelRandom final {
public:
    explicit ModelRandom(const std::uint64_t seed)
        : state_(seed == 0 ? 0x9e3779b97f4a7c15ULL : seed) {}

    std::uint64_t next() {
        state_ ^= state_ >> 12U;
        state_ ^= state_ << 25U;
        state_ ^= state_ >> 27U;
        return state_ * 2685821657736338717ULL;
    }

private:
    std::uint64_t state_;
};

ModelSchedule generate_model_schedule(
    const std::uint64_t seed,
    const ModelSchedule::Profile profile = ModelSchedule::Profile::Ci) {
    ModelRandom random(seed);
    ModelSchedule schedule{};
    schedule.seed = seed;
    schedule.profile = profile;
    schedule.cut = static_cast<ModelCut>(random.next() % 11);
    const auto cut = static_cast<std::uint8_t>(schedule.cut);
    if (cut < static_cast<std::uint8_t>(ModelCut::ManifestAttempt)) {
        const auto completed = std::min<std::uint8_t>(cut, 5);
        schedule.journal_regions = static_cast<std::uint8_t>(
            (1U << completed) - 1U);
        if ((random.next() & 1U) != 0 && completed < 5) {
            schedule.journal_regions |= static_cast<std::uint8_t>(
                1U << (random.next() % 5));
        }
        schedule.tail_garbage = (random.next() & 1U) != 0;
    } else {
        schedule.journal_regions = 0x1f;
        if (schedule.cut == ModelCut::ManifestAttempt) {
            constexpr std::array<std::uint16_t, 4> lengths{
                0, 7, 16, kModelManifestBytes};
            schedule.manifest_bytes = lengths[random.next() % lengths.size()];
            schedule.tail_garbage = (random.next() & 1U) != 0;
        } else {
            schedule.manifest_bytes = kModelManifestBytes;
        }
    }
    return schedule;
}

std::uint64_t derive_m3_seed(const std::uint64_t index) {
    const std::string material =
        std::string{MYRD_M3_TASK33_SOURCE_COMMIT} + "|33|" +
        std::to_string(index);
    BCRYPT_ALG_HANDLE algorithm{};
    if (BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
        throw std::runtime_error("m3-seed-v1 cannot open SHA-256");
    }
    std::array<UCHAR, 32> digest{};
    const auto status = BCryptHash(
        algorithm, nullptr, 0,
        reinterpret_cast<PUCHAR>(
            const_cast<char*>(material.data())),
        static_cast<ULONG>(material.size()), digest.data(),
        static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0) {
        throw std::runtime_error("m3-seed-v1 SHA-256 failed");
    }
    std::uint64_t low{};
    for (std::size_t index_in_digest = 24;
         index_in_digest < digest.size(); ++index_in_digest) {
        low = (low << 8U) | digest[index_in_digest];
    }
    return low;
}

std::uint64_t requested_schedule_count() {
    std::array<wchar_t, 32> value{};
    const auto length = GetEnvironmentVariableW(
        L"MYRD_M3_CRASH_MODEL_SCHEDULES", value.data(),
        static_cast<DWORD>(value.size()));
    if (length == 0) return 10'000;
    if (length >= value.size()) {
        throw std::runtime_error("crash-model schedule count is too long");
    }
    const auto parsed = std::stoull(value.data());
    return std::max<std::uint64_t>(10'000, parsed);
}

ModelClone clone_for_schedule(const ModelSchedule& schedule) {
    const auto complete_journal = complete_model_journal();
    const auto complete_manifest = complete_model_manifest(complete_journal);
    ModelClone clone{};
    clone.journal.resize(kModelJournalBytes);
    constexpr std::array<std::pair<std::size_t, std::size_t>, 5> regions{{
        {0, kModelRecordBytes},
        {kModelRecordBytes, kModelRecordBytes},
        {2 * kModelRecordBytes, kModelRecordBytes},
        {kModelIndexOffset, kModelRecordBytes},
        {kModelCommitOffset, kModelRecordBytes}}};
    for (std::size_t index = 0; index < regions.size(); ++index) {
        if ((schedule.journal_regions & (1U << index)) == 0) continue;
        const auto [offset, length] = regions[index];
        std::copy_n(complete_journal.begin() +
                        static_cast<std::ptrdiff_t>(offset),
                    static_cast<std::ptrdiff_t>(length),
                    clone.journal.begin() +
                        static_cast<std::ptrdiff_t>(offset));
    }
    if (schedule.tail_garbage) {
        clone.journal.insert(clone.journal.end(), 23,
                             std::byte{0xdc});
    }
    clone.manifest.resize(schedule.manifest_bytes);
    std::copy_n(complete_manifest.begin(),
                static_cast<std::ptrdiff_t>(schedule.manifest_bytes),
                clone.manifest.begin());
    return clone;
}

std::string model_cut_name(const ModelCut cut) {
    switch (cut) {
    case ModelCut::DataWrite: return "journal.data";
    case ModelCut::ZeroWrite: return "journal.zero";
    case ModelCut::DiscardWrite: return "journal.discard";
    case ModelCut::IndexWrite: return "journal.index";
    case ModelCut::CommitWrite: return "journal.commit";
    case ModelCut::JournalFlush: return "journal.flush";
    case ModelCut::JournalReadback: return "journal.readback";
    case ModelCut::ManifestAttempt: return "manifest.attempt";
    case ModelCut::ManifestFlush: return "manifest.flush";
    case ModelCut::Install: return "backing.install";
    case ModelCut::Ack: return "backing.ack";
    }
    return "unknown";
}

std::string hex_hash(const std::span<const std::byte> bytes) {
    std::ostringstream output;
    output << std::hex << std::setw(16) << std::setfill('0')
           << independent_hash(bytes);
    return output.str();
}

bool model_recovery_is_complete(const ModelRecovery& recovered) {
    const auto expected = recovered.generation == ModelGeneration::Old
        ? old_model_map()
        : new_model_map();
    return recovered.map == expected;
}

bool model_clone_matches_schedule(const ModelSchedule& schedule,
                                  const ModelClone& clone) {
    const auto recovered = recover_model_clone(clone);
    const bool before_manifest =
        schedule.cut < ModelCut::ManifestAttempt;
    const bool indeterminate =
        schedule.cut == ModelCut::ManifestAttempt;
    const bool generation_allowed = before_manifest
        ? recovered.generation == ModelGeneration::Old
        : indeterminate
            ? true
            : recovered.generation == ModelGeneration::New;
    return !schedule.ack_before_manifest && generation_allowed &&
           model_recovery_is_complete(recovered);
}

ModelSchedule shrink_failing_schedule(ModelSchedule reduced) {
    const auto keep_if_still_failing = [&reduced](
        const ModelSchedule& candidate) {
        if (!model_clone_matches_schedule(
                candidate, clone_for_schedule(candidate))) {
            reduced = candidate;
        }
    };
    if (reduced.tail_garbage) {
        auto candidate = reduced;
        candidate.tail_garbage = false;
        keep_if_still_failing(candidate);
    }
    for (std::uint8_t bit = 0; bit < 5; ++bit) {
        if ((reduced.journal_regions & (1U << bit)) == 0) continue;
        auto candidate = reduced;
        candidate.journal_regions &= static_cast<std::uint8_t>(~(1U << bit));
        keep_if_still_failing(candidate);
    }
    constexpr std::array<std::uint16_t, 4> shorter_manifest{
        0, 1, 7, 16};
    for (const auto length : shorter_manifest) {
        if (length >= reduced.manifest_bytes) continue;
        auto candidate = reduced;
        candidate.manifest_bytes = length;
        keep_if_still_failing(candidate);
    }
    if (reduced.ack_before_manifest) {
        auto candidate = reduced;
        candidate.ack_before_manifest = false;
        keep_if_still_failing(candidate);
    }
    return reduced;
}

std::uint64_t model_operation_count(const ModelSchedule& schedule) {
    std::uint64_t count{};
    for (std::uint8_t bit = 0; bit < 5; ++bit) {
        if ((schedule.journal_regions & (1U << bit)) != 0) ++count;
    }
    count += schedule.manifest_bytes != 0 ? 1 : 0;
    count += schedule.tail_garbage ? 1 : 0;
    count += schedule.ack_before_manifest ? 1 : 0;
    return count;
}

std::string profile_name(const ModelSchedule::Profile profile) {
    switch (profile) {
    case ModelSchedule::Profile::Quick: return "quick";
    case ModelSchedule::Profile::Ci: return "ci";
    case ModelSchedule::Profile::Manual: return "manual";
    }
    return "unknown";
}

std::optional<std::string> evaluate_model_schedule(
    const ModelSchedule& schedule) {
    const auto clone = clone_for_schedule(schedule);
    if (model_clone_matches_schedule(schedule, clone)) return std::nullopt;
    const auto minimal = shrink_failing_schedule(schedule);
    const auto minimal_clone = clone_for_schedule(minimal);
    std::ostringstream message;
    message << "schema=m3-seed-v1"
            << ";source_commit=" << MYRD_M3_TASK33_SOURCE_COMMIT
            << ";card_id=33"
            << ";seed=" << schedule.seed
            << ";profile=" << profile_name(schedule.profile)
            << ";operation_count=" << model_operation_count(schedule)
            << ";phase=" << model_cut_name(schedule.cut)
            << ";minimal_trace=regions:0x" << std::hex
            << static_cast<unsigned>(minimal.journal_regions)
            << ",manifest_bytes:" << std::dec
            << minimal.manifest_bytes
            << ",tail:" << minimal.tail_garbage
            << ",ack_before_manifest:" << minimal.ack_before_manifest
            << ",operations:" << model_operation_count(minimal)
            << ";journal_hash=" << hex_hash(minimal_clone.journal)
            << ";manifest_hash=" << hex_hash(minimal_clone.manifest);
    return message.str();
}

TEST_CASE(checkpoint_crash_matrix_fixture_is_present_and_versioned) {
    const auto json = load_matrix_json();
    REQUIRE(json.find("\"schema_version\": 1") != std::string::npos ||
            json.find("\"schema_version\":1") != std::string::npos);
    const auto scenarios = parse_matrix(json);
    REQUIRE_EQ(count_occurrences(json, "\"id\""), scenarios.size());
    const auto registry_json =
        load_text(MYRD_TEST_PATH(MYRD_M3_FAULT_PHASE_REGISTRY_PATH));
    const auto registry = parse_registry(registry_json);
    REQUIRE_EQ(count_occurrences(registry_json, "\"id\""),
               registry.size());
    REQUIRE(matrix_matches_transaction_rules(scenarios, registry));
}

TEST_CASE(checkpoint_crash_matrix_rejects_classification_and_order_mutations) {
    const auto scenarios = parse_matrix(load_matrix_json());
    const auto registry = parse_registry(
        load_text(MYRD_TEST_PATH(MYRD_M3_FAULT_PHASE_REGISTRY_PATH)));
    REQUIRE(matrix_matches_transaction_rules(scenarios, registry));

    auto wrong_commit = scenarios;
    const auto committed = std::find_if(
        wrong_commit.begin(), wrong_commit.end(), [](const Scenario& item) {
            return item.id == "manifest.flush.after";
        });
    REQUIRE(committed != wrong_commit.end());
    committed->allowed_recovery = "old_or_new";
    REQUIRE(!matrix_matches_transaction_rules(wrong_commit, registry));

    auto wrong_ack_order = scenarios;
    const auto ack = std::find_if(
        wrong_ack_order.begin(), wrong_ack_order.end(),
        [](const Scenario& item) {
            return starts_with(item.id, "backing.ack.each.");
        });
    REQUIRE(ack != wrong_ack_order.end());
    std::iter_swap(wrong_ack_order.begin(), ack);
    REQUIRE(!matrix_matches_transaction_rules(wrong_ack_order, registry));
}

TEST_CASE(checkpoint_crash_clones_recover_only_old_or_new_complete_maps) {
    const auto scenarios = parse_matrix(load_matrix_json());
    const auto registry = parse_registry(
        load_text(MYRD_TEST_PATH(MYRD_M3_FAULT_PHASE_REGISTRY_PATH)));
    REQUIRE(matrix_matches_transaction_rules(scenarios, registry));
    CrashBundle bundle;
    for (const auto& scenario : scenarios) {
        if (scenario.allowed_recovery == "not_applicable") continue;
        bundle.install_clone(scenario);
        const auto recovered = bundle.recover();
        if (!recovered.ok() || !recovered.image.has_value()) {
            throw std::runtime_error(
                "crash clone is not recoverable: " + scenario.id);
        }
        if (!generation_is_allowed(
                scenario, recovered.image->checkpoint_generation, bundle)) {
            throw std::runtime_error(
                "crash clone selected a forbidden generation: " +
                scenario.id);
        }
        require_complete_map(*recovered.image, bundle);
    }
}

TEST_CASE(checkpoint_crash_clones_cover_short_and_all_sector_torn_cuts) {
    const auto scenarios = parse_matrix(load_matrix_json());
    const auto registry = parse_registry(
        load_text(MYRD_TEST_PATH(MYRD_M3_FAULT_PHASE_REGISTRY_PATH)));
    CrashBundle bundle;
    for (const auto& phase : registry) {
        if (!phase.short_write || !phase.kill) continue;
        const auto found = std::find_if(
            scenarios.begin(), scenarios.end(),
            [&phase](const Scenario& item) {
                return item.id == phase.id;
            });
        if (found == scenarios.end()) continue;
        const auto length = bundle.journal_target_length(*found);
        if (length <= 1) continue;
        std::set<std::uint64_t> cuts{
            1, std::min<std::uint64_t>(31, length - 1),
            std::min<std::uint64_t>(32, length - 1),
            std::min<std::uint64_t>(4096, length - 1),
            length / 2, length - 1};
        for (const auto cut : cuts) {
            bundle.install_journal_short_clone(*found, cut);
            const auto recovered = bundle.recover();
            REQUIRE(recovered.ok());
            REQUIRE(recovered.image.has_value());
            REQUIRE_EQ(recovered.image->checkpoint_generation,
                       bundle.old_generation());
            require_complete_map(*recovered.image, bundle);
        }
    }
    const auto torn = std::find_if(
        scenarios.begin(), scenarios.end(), [](const Scenario& item) {
            return item.id == "manifest.slot_write.torn";
        });
    REQUIRE(torn != scenarios.end());
    for (std::size_t cut = 512; cut < kManifestSlotBytes; cut += 512) {
        bundle.install_manifest_torn_clone(cut);
        const auto recovered = bundle.recover();
        REQUIRE(recovered.ok());
        REQUIRE(recovered.image.has_value());
        REQUIRE(generation_is_allowed(
            *torn, recovered.image->checkpoint_generation, bundle));
        require_complete_map(*recovered.image, bundle);
    }
}

TEST_CASE(checkpoint_crash_model_runs_ten_thousand_deterministic_schedules) {
    const auto schedule_count = requested_schedule_count();
    constexpr std::array<std::uint64_t, 4> quick_seeds{
        0, 1, 0x00C0FFEEULL, 0xDEADBEEFULL};
    for (const auto seed : quick_seeds) {
        const auto schedule = generate_model_schedule(
            seed, ModelSchedule::Profile::Quick);
        const auto failure = evaluate_model_schedule(schedule);
        if (failure.has_value()) throw std::runtime_error(*failure);
    }
    const auto bulk_profile = schedule_count > 10'000
        ? ModelSchedule::Profile::Manual
        : ModelSchedule::Profile::Ci;
    for (std::uint64_t index = 0;
         index < schedule_count - quick_seeds.size(); ++index) {
        const auto schedule = generate_model_schedule(
            derive_m3_seed(index), bulk_profile);
        const auto failure = evaluate_model_schedule(schedule);
        if (failure.has_value()) throw std::runtime_error(*failure);
    }
    const auto derived = derive_m3_seed(117);
    const auto first = generate_model_schedule(derived, bulk_profile);
    const auto replay = generate_model_schedule(derived, bulk_profile);
    REQUIRE_EQ(derived, derive_m3_seed(117));
    REQUIRE(derived != derive_m3_seed(118));
    REQUIRE_EQ(first.cut, replay.cut);
    REQUIRE_EQ(first.journal_regions, replay.journal_regions);
    REQUIRE_EQ(first.manifest_bytes, replay.manifest_bytes);
    REQUIRE_EQ(first.tail_garbage, replay.tail_garbage);
}

TEST_CASE(checkpoint_crash_model_detects_commit_flush_ack_and_mixed_mutations) {
    ModelSchedule missing_manifest{};
    missing_manifest.seed = 0xBAD3301;
    missing_manifest.cut = ModelCut::ManifestFlush;
    missing_manifest.journal_regions = 0x1f;
    missing_manifest.manifest_bytes = 0;
    const auto missing_failure =
        evaluate_model_schedule(missing_manifest);
    REQUIRE(missing_failure.has_value());
    REQUIRE(missing_failure->find("schema=m3-seed-v1") !=
            std::string::npos);
    REQUIRE(missing_failure->find("source_commit=") !=
            std::string::npos);
    REQUIRE(missing_failure->find("profile=ci") != std::string::npos);
    REQUIRE(missing_failure->find("operation_count=") !=
            std::string::npos);
    REQUIRE(missing_failure->find("minimal_trace=") !=
            std::string::npos);
    REQUIRE(missing_failure->find("journal_hash=") !=
            std::string::npos);
    REQUIRE(missing_failure->find("manifest_hash=") !=
            std::string::npos);

    ModelSchedule reordered_flush{};
    reordered_flush.seed = 0xBAD3302;
    reordered_flush.cut = ModelCut::ManifestFlush;
    reordered_flush.journal_regions = 0x07;
    reordered_flush.manifest_bytes = kModelManifestBytes;
    REQUIRE(evaluate_model_schedule(reordered_flush).has_value());
    const auto reduced_flush = shrink_failing_schedule(reordered_flush);
    REQUIRE(model_operation_count(reduced_flush) <
            model_operation_count(reordered_flush));
    REQUIRE(!model_clone_matches_schedule(
        reduced_flush, clone_for_schedule(reduced_flush)));

    ModelSchedule early_ack{};
    early_ack.seed = 0xBAD3303;
    early_ack.cut = ModelCut::CommitWrite;
    early_ack.journal_regions = 0x1f;
    early_ack.ack_before_manifest = true;
    REQUIRE(evaluate_model_schedule(early_ack).has_value());
    const auto reduced_ack = shrink_failing_schedule(early_ack);
    REQUIRE(reduced_ack.ack_before_manifest);
    REQUIRE(!model_clone_matches_schedule(
        reduced_ack, clone_for_schedule(reduced_ack)));

    ModelSchedule mixed{};
    mixed.seed = 0xBAD3304;
    mixed.cut = ModelCut::ManifestFlush;
    mixed.journal_regions = 0x1f;
    mixed.manifest_bytes = kModelManifestBytes;
    auto clone = clone_for_schedule(mixed);
    clone.journal[kModelRecordBytes + 3] ^= std::byte{1};
    const auto recovered = recover_model_clone(clone);
    REQUIRE_EQ(recovered.generation, ModelGeneration::Old);
    REQUIRE(recovered.map == old_model_map());
    REQUIRE(!model_clone_matches_schedule(mixed, clone));

    auto mixed_recovery = ModelRecovery{
        ModelGeneration::New, new_model_map()};
    mixed_recovery.map[4] ^= std::byte{1};
    REQUIRE(!model_recovery_is_complete(mixed_recovery));
    REQUIRE(evaluate_model_schedule(ModelSchedule{
        0xBAD3305, ModelCut::ManifestFlush, 0x0f,
        kModelManifestBytes, false, false}).has_value());
}

}  // namespace
}  // namespace myrd::persistence
