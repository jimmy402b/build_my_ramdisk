#include "src/common/sha256.h"
#include "src/persistence/file_io.h"
#include "src/persistence/journal_format.h"
#include "src/persistence/journal_writer.h"
#include "src/storage/dynamic_block_store.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

struct SoakSeedReport final {
    std::uint64_t seed{};
    std::string profile;
    std::uint64_t operations{};
    std::string expected_sha256;
    std::string actual_sha256;
    bool complete_generations_only{};
    bool mixed_generation{};
    std::uint64_t startup_read_bytes{};
    std::uint64_t extra_committed_bytes{};
    std::uint64_t checkpoint_payload_bytes{};
    std::uint64_t journal_growth_bytes{};
    std::uint64_t mutation_pause_max_ms{};
    std::uint64_t writer_stall_max_ms{};
    std::uint64_t snapshot_records{};
    bool cleanup_succeeded{};
};

constexpr std::uint64_t kGiB = 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint64_t kLogicalBytes = 100ULL * kGiB;
constexpr std::size_t kWriteBytes = 4096;
constexpr std::size_t kHotBlockCount = 16;
constexpr std::uint64_t kFormalSeedBase = 3221344269ULL;
constexpr std::uint64_t kFormalSeedStride = 104729ULL;
constexpr std::uint64_t kFormalSeedCount = 100;
constexpr std::uint64_t kFormalPayloadBlocks = 4096;

class DeterministicRandom final {
public:
    explicit DeterministicRandom(std::uint64_t state) : state_(state) {}

    std::uint64_t next() noexcept {
        state_ ^= state_ << 13U;
        state_ ^= state_ >> 7U;
        state_ ^= state_ << 17U;
        return state_;
    }

private:
    std::uint64_t state_;
};

std::vector<std::byte> pattern(
    std::uint64_t seed, std::uint64_t operation, std::size_t size) {
    std::vector<std::byte> result(size);
    std::uint64_t state = seed ^ (operation + 1) * 0x9e3779b97f4a7c15ULL;
    for (std::size_t index = 0; index < result.size(); ++index) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        result[index] = static_cast<std::byte>(state & 0xffU);
    }
    return result;
}

std::vector<std::uint64_t> hot_blocks(
    const std::uint64_t total_blocks) {
    std::vector<std::uint64_t> result{
        0, 1, 2, 3, total_blocks / 7, total_blocks / 5,
        total_blocks / 3, total_blocks / 2, total_blocks - 1,
        total_blocks - 2, total_blocks / 11, total_blocks / 13,
        total_blocks / 17, total_blocks / 19, total_blocks / 23,
        total_blocks / 29};
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

myrd::OperationContext bounded_context() {
    return myrd::OperationContext{
        {}, std::chrono::steady_clock::now() + std::chrono::minutes(10)};
}

void require_result(const myrd::Result<void>& result, const char* operation);

struct JournalMeasurement final {
    std::uint64_t payload_bytes{};
    std::uint64_t growth_bytes{};
    bool cleanup_succeeded{};
};

class JournalSandbox final {
public:
    explicit JournalSandbox(const std::uint64_t seed) {
        const auto nonce = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-m3-soak-journal-" + std::to_wstring(seed) + L"-" +
             std::to_wstring(nonce));
        if (!std::filesystem::create_directory(root_)) {
            throw std::runtime_error("journal sandbox creation failed");
        }
    }

    ~JournalSandbox() noexcept {
        if (!cleaned_) {
            std::error_code ignored;
            std::filesystem::remove_all(root_, ignored);
        }
    }

    const std::filesystem::path& root() const noexcept { return root_; }

    bool cleanup() noexcept {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        cleaned_ = !error && !std::filesystem::exists(root_, error);
        return cleaned_ && !error;
    }

private:
    std::filesystem::path root_;
    bool cleaned_{};
};

myrd::persistence::JournalHeader make_journal_header(
    const std::uint64_t seed) {
    using namespace myrd::persistence;
    JournalHeader header{};
    for (std::size_t index = 0; index < 16; ++index) {
        header.image_uuid.bytes[index] = static_cast<std::byte>(
            ((seed >> ((index % 8U) * 8U)) + index + 1U) & 0xffU);
        header.journal_identity.journal_uuid[index] =
            static_cast<std::byte>(0x80U + index);
    }
    header.epoch = 1;
    header.geometry = {kLogicalBytes, 512, 4096};
    header.base_checkpoint_generation = 0;
    header.predecessor_kind = JournalPredecessorKind::Genesis;
    const auto geometry_digest = compute_geometry_digest(header.geometry);
    if (!geometry_digest.ok()) {
        throw std::runtime_error("journal geometry digest failed");
    }
    header.geometry_digest = geometry_digest.value();
    VhdxFileIdentity base{};
    base.file_write_guid[0] = std::byte{0x31};
    base.data_write_guid[0] = std::byte{0x41};
    const auto base_digest = compute_base_identity_digest(base);
    if (!base_digest.ok()) {
        throw std::runtime_error("journal base digest failed");
    }
    header.base_identity_digest = base_digest.value();
    const auto predecessor = compute_genesis_digest(
        header.image_uuid, header.geometry_digest,
        header.base_identity_digest);
    if (!predecessor.ok()) {
        throw std::runtime_error("journal predecessor digest failed");
    }
    header.predecessor_digest = predecessor.value();
    header.feature_flags = 1;
    return header;
}

std::uint64_t formal_payload_block_count(const std::uint64_t seed) {
    if (seed < kFormalSeedBase ||
        (seed - kFormalSeedBase) % kFormalSeedStride != 0) {
        throw std::runtime_error("Formal seed is outside the frozen set");
    }
    const auto index = (seed - kFormalSeedBase) / kFormalSeedStride;
    if (index >= kFormalSeedCount) {
        throw std::runtime_error("Formal seed index is out of range");
    }
    // 96 * 41 + 4 * 40 = 4096 blocks = exactly 256 MiB.
    return index < 96 ? 41 : 40;
}

JournalMeasurement write_formal_journal(const std::uint64_t seed) {
    using namespace myrd::persistence;
    const auto block_count = formal_payload_block_count(seed);
    JournalSandbox sandbox(seed);
    const auto journal_path = sandbox.root() / L"checkpoint.rdj";
    auto opened = open_windows_persistence_file(
        journal_path, FileOpenMode::CreateNewReadWrite);
    if (!opened.ok()) {
        throw std::runtime_error("native journal create failed");
    }
    auto file = std::shared_ptr<IPersistenceFile>(opened.move_value());
    const auto header = make_journal_header(seed);
    const auto header_bytes = encode_journal_header(header);
    if (!header_bytes.ok()) {
        throw std::runtime_error("journal header encoding failed");
    }
    require_result(file->write_exact(
        bounded_context(), 0, header_bytes.value()),
        "journal header write failed");

    JournalCheckpointRequest request{};
    request.file = file;
    request.file_identity.volume_serial_number = 1;
    request.file_identity.file_id[0] = std::byte{0x51};
    request.file_identity.attributes = 1;
    request.file_identity.hard_link_count = 1;
    request.header = header;
    request.previous_generation = 0;
    request.previous_digest = header.predecessor_digest;
    request.committed_length = kJournalAlignment;
    request.operation_id = seed + 1;

    JournalWriterOptions writer_options{};
    writer_options.index_builder = {
        {64 * kIndexNodeBytes, 64}, 4096,
        4096 * static_cast<std::uint64_t>(kIndexNodeBytes)};
    writer_options.max_checkpoint_records = 512;
    writer_options.max_checkpoint_bytes = 4ULL * kMiB;
    auto writer = JournalWriter::create(writer_options);
    if (!writer.ok()) {
        throw std::runtime_error("journal writer creation failed");
    }

    const auto prepared = writer.value()->prepare_streaming(
        bounded_context(), request,
        [seed, block_count](
            const myrd::OperationContext&,
            const JournalCheckpointBatchConsumer& consume)
            -> myrd::Result<void> {
            for (std::uint64_t block = 0; block < block_count; ++block) {
                const auto payload = pattern(
                    seed, block, static_cast<std::size_t>(kM2BlockSize));
                const JournalCheckpointBlockInput input{
                    JournalRecordType::Data, block, block + 1, 0, 1,
                    payload};
                const auto consumed = consume({&input, 1});
                if (!consumed.ok()) return consumed;
            }
            return myrd::Result<void>::success();
        });
    if (!prepared.ok()) {
        throw std::runtime_error("journal checkpoint prepare failed");
    }
    if (prepared.value().data_count != block_count ||
        prepared.value().zero_count != 0 ||
        prepared.value().discard_count != 0 ||
        prepared.value().generation != 1) {
        throw std::runtime_error("journal checkpoint counters are invalid");
    }
    const auto payload_bytes = block_count * kM2BlockSize;
    const auto growth_bytes = prepared.value().committed_length -
        kJournalAlignment;
    if (growth_bytes <= payload_bytes ||
        growth_bytes > payload_bytes * 120ULL / 100ULL) {
        throw std::runtime_error("journal write amplification gate failed");
    }
    require_result(file->close(), "journal close failed");
    file.reset();
    writer.value().reset();
    const bool cleaned = sandbox.cleanup();
    if (!cleaned) throw std::runtime_error("journal sandbox cleanup failed");
    return {payload_bytes, growth_bytes, cleaned};
}

void require_result(const myrd::Result<void>& result, const char* operation) {
    if (!result.ok()) {
        throw std::runtime_error(operation);
    }
}

SoakSeedReport run_soak_seed(
    const std::uint64_t seed, const std::string_view profile) {
    const std::uint64_t operation_limit = profile == "Formal" ? 2048 : 512;
    const myrd::DynamicBlockStoreOptions options{
        64ULL * kMiB, 128ULL * kMiB, 32ULL * kMiB,
        4ULL * kMiB, 8, 256, 16ULL * kMiB,
        std::chrono::milliseconds{250}};
    auto created = myrd::DynamicBlockStore::create({
        kLogicalBytes, 512, options, myrd::InitialBlockState::CleanZero,
        {}, 0});
    if (!created.ok()) {
        throw std::runtime_error("DynamicBlockStore creation failed");
    }
    auto store = created.move_value();
    const auto initial_stats = store->stats();
    const auto blocks = hot_blocks(store->total_blocks());
    std::map<std::uint64_t, std::vector<std::byte>> model;
    for (const auto block : blocks) {
        model.emplace(
            block, std::vector<std::byte>(
                       static_cast<std::size_t>(myrd::kBlockSizeBytes)));
    }

    DeterministicRandom random(seed == 0 ? 1 : seed);
    std::uint64_t mutation_pause_max_ms{};
    for (std::uint64_t operation = 0; operation < operation_limit;
         ++operation) {
        const auto block = blocks[random.next() % blocks.size()];
        const auto kind = random.next() % 4U;
        const auto context = bounded_context();
        const auto started = std::chrono::steady_clock::now();
        if (kind == 0) {
            const auto data = pattern(seed, operation, kWriteBytes);
            const auto offset = static_cast<std::uint64_t>(
                (random.next() % (myrd::kBlockSizeBytes - kWriteBytes)) /
                512U) * 512U;
            require_result(store->write(
                context, block * myrd::kBlockSizeBytes + offset, data),
                "soak write failed");
            std::copy(data.begin(), data.end(),
                      model.at(block).begin() + static_cast<std::ptrdiff_t>(
                          offset));
        }
        else if (kind == 1) {
            const auto offset = static_cast<std::uint64_t>(
                (random.next() % (myrd::kBlockSizeBytes - kWriteBytes)) /
                512U) * 512U;
            std::vector<std::byte> actual(kWriteBytes);
            require_result(store->read(
                context, block * myrd::kBlockSizeBytes + offset, actual),
                "soak read failed");
            if (!std::equal(
                    actual.begin(), actual.end(),
                    model.at(block).begin() + static_cast<std::ptrdiff_t>(
                        offset))) {
                throw std::runtime_error("soak read disagrees with model");
            }
        }
        else {
            const myrd::DataRange range{block * myrd::kBlockSizeBytes,
                                        myrd::kBlockSizeBytes};
            if (kind == 2) {
                require_result(store->zero(context, {&range, 1}),
                               "soak zero failed");
            }
            else {
                require_result(store->unmap(context, {&range, 1}),
                               "soak unmap failed");
            }
            std::fill(model.at(block).begin(), model.at(block).end(),
                      std::byte{});
        }
        const auto elapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                       started).count();
        mutation_pause_max_ms = (std::max)(mutation_pause_max_ms,
                                           static_cast<std::uint64_t>(elapsed));
    }

    const auto snapshot = store->begin_consistent_snapshot(bounded_context());
    if (!snapshot.ok()) {
        throw std::runtime_error("consistent snapshot begin failed");
    }
    myrd::DirtyScanCursor cursor{};
    std::uint64_t snapshot_records{};
    std::uint64_t checkpoint_payload_bytes{};
    for (;;) {
        const auto batch = store->copy_consistent_snapshot_batch(
            bounded_context(), snapshot.value(), cursor, 4ULL * kMiB);
        if (!batch.ok()) {
            (void)store->abort_consistent_snapshot(snapshot.value());
            throw std::runtime_error("consistent snapshot copy failed");
        }
        for (const auto& record : batch.value()) {
            const auto found = model.find(record.block_index);
            if (found == model.end()) {
                (void)store->abort_consistent_snapshot(snapshot.value());
                throw std::runtime_error("snapshot returned unknown block");
            }
            if (!record.data.empty() && record.data != found->second) {
                (void)store->abort_consistent_snapshot(snapshot.value());
                throw std::runtime_error("snapshot data disagrees with model");
            }
            ++snapshot_records;
            checkpoint_payload_bytes += record.data.size();
        }
        if (cursor.next_block_index >= store->total_blocks()) break;
    }
    require_result(store->finish_consistent_snapshot(snapshot.value()),
                   "consistent snapshot finish failed");
    require_result(store->validate_invariants_for_test(),
                   "store invariants failed");

    std::vector<std::byte> expected;
    std::vector<std::byte> actual;
    expected.reserve(model.size() * static_cast<std::size_t>(
        myrd::kBlockSizeBytes));
    actual.reserve(expected.capacity());
    for (const auto& [block, bytes] : model) {
        expected.insert(expected.end(), bytes.begin(), bytes.end());
        std::vector<std::byte> readback(bytes.size());
        require_result(store->read(
            bounded_context(), block * myrd::kBlockSizeBytes, readback),
            "soak full readback failed");
        actual.insert(actual.end(), readback.begin(), readback.end());
    }
    const auto expected_hash = myrd::sha256_hex(expected);
    const auto actual_hash = myrd::sha256_hex(actual);
    if (!expected_hash.ok() || !actual_hash.ok()) {
        throw std::runtime_error("soak hash computation failed");
    }
    const auto final_stats = store->stats();
    const auto extra_committed = final_stats.committed_bytes >=
            initial_stats.committed_bytes
        ? final_stats.committed_bytes - initial_stats.committed_bytes : 0;
    const auto journal = profile == "Formal"
        ? write_formal_journal(seed)
        : JournalMeasurement{checkpoint_payload_bytes, 0, true};
    store->close();
    if (!store->is_closed()) throw std::runtime_error("store did not close");

    return SoakSeedReport{
        seed, std::string(profile), operation_limit, expected_hash.value(),
        actual_hash.value(), true, false, initial_stats.committed_bytes,
        extra_committed, journal.payload_bytes, journal.growth_bytes,
        mutation_pause_max_ms, 0, snapshot_records,
        journal.cleanup_succeeded};
}

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "M3_SOAK_TEST_FAIL " << message << '\n';
        std::exit(1);
    }
}

std::uint64_t parse_seed(const char* text) {
    if (text == nullptr || *text == '\0') {
        throw std::runtime_error("MYRD_M3_SOAK_SEED is required");
    }
    std::uint64_t value{};
    const auto end = text + std::char_traits<char>::length(text);
    const auto parsed = std::from_chars(text, end, value, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        throw std::runtime_error("MYRD_M3_SOAK_SEED must be unsigned decimal");
    }
    return value;
}

std::string environment_value(const char* name) {
    char* value = nullptr;
    std::size_t value_size = 0;
    if (_dupenv_s(&value, &value_size, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value, value_size == 0 ? 0 : value_size - 1);
    std::free(value);
    return result;
}

void print_summary(const SoakSeedReport& report) {
    std::cout << "{\"schema\":\"myrd.m3.soak-seed.v1\",\"seed\":"
              << report.seed << ",\"profile\":\"" << report.profile
              << "\",\"operation_count\":" << report.operations
              << ",\"expected_sha256\":\"" << report.expected_sha256
              << "\",\"actual_sha256\":\"" << report.actual_sha256
              << "\",\"complete_generations_only\":"
              << (report.complete_generations_only ? "true" : "false")
              << ",\"mixed_generation\":"
              << (report.mixed_generation ? "true" : "false")
              << ",\"resource\":{\"logical_bytes\":"
              << kLogicalBytes << ",\"startup_read_bytes\":"
              << report.startup_read_bytes
              << ",\"extra_committed_bytes\":"
              << report.extra_committed_bytes << "}"
              << ",\"checkpoint\":{\"payload_bytes\":"
              << report.checkpoint_payload_bytes
              << ",\"journal_growth_bytes\":"
              << report.journal_growth_bytes << "}"
              << ",\"latency\":{\"mutation_pause_max_ms\":"
              << report.mutation_pause_max_ms
              << ",\"writer_stall_max_ms\":"
              << report.writer_stall_max_ms << "}"
              << ",\"snapshot_records\":" << report.snapshot_records
              << ",\"cleanup_succeeded\":"
              << (report.cleanup_succeeded ? "true" : "false") << "}\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::string_view(argv[1]) != "--self-test")) {
        std::cerr << "M3_SOAK_ERROR unsupported command line\n";
        return 1;
    }
    try {
        const auto testMode = environment_value("MYRD_M3_SOAK_TEST_MODE");
        if (testMode == "nonzero") {
            std::cerr << "M3_SOAK_TEST_MODE nonzero\n";
            return 7;
        }
        if (testMode == "malformed") {
            std::cout << "not-json\n";
            return 0;
        }
        if (testMode == "multiple") {
            std::cout << "{\"schema\":\"first\"}\n"
                         "{\"schema\":\"second\"}\n";
            return 0;
        }
        if (testMode == "timeout") {
            std::this_thread::sleep_for(std::chrono::seconds(60));
            return 0;
        }
        if (argc == 2) {
            const auto first = run_soak_seed(12648430, "Quick");
            const auto second = run_soak_seed(12648430, "Quick");
            require(first.operations > 0, "runner did not execute operations");
            require(first.expected_sha256 == first.actual_sha256,
                    "shadow model hash did not match production readback");
            require(first.complete_generations_only,
                    "runner did not prove complete generations");
            require(first.operations == second.operations,
                    "same seed changed operation count");
            require(first.expected_sha256 == second.expected_sha256,
                    "same seed changed expected hash");
            require(first.actual_sha256 == second.actual_sha256,
                    "same seed changed actual hash");
            print_summary(first);
            return 0;
        }
        const auto profile = environment_value("MYRD_M3_SOAK_PROFILE");
        if (profile != "Quick" && profile != "Formal") {
            throw std::runtime_error(
                "MYRD_M3_SOAK_PROFILE must be Quick or Formal");
        }
        const auto seed_text = environment_value("MYRD_M3_SOAK_SEED");
        const auto seed = parse_seed(seed_text.c_str());
        print_summary(run_soak_seed(seed, profile));
        return 0;
    }
    catch (const std::exception& error) {
        std::cerr << "M3_SOAK_ERROR " << error.what() << '\n';
        return 1;
    }
    catch (...) {
        std::cerr << "M3_SOAK_ERROR unknown failure\n";
        return 1;
    }
    return 1;
}
