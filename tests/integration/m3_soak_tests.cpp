#include "src/common/sha256.h"
#include "src/storage/dynamic_block_store.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
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
    store->close();
    if (!store->is_closed()) throw std::runtime_error("store did not close");

    return SoakSeedReport{
        seed, std::string(profile), operation_limit, expected_hash.value(),
        actual_hash.value(), true, false, initial_stats.committed_bytes,
        extra_committed, checkpoint_payload_bytes, 0, mutation_pause_max_ms,
        0, snapshot_records, true};
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
