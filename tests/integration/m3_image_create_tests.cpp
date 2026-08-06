#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "src/persistence/image_creator.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/manifest_format.h"
#include "src/persistence/persistence_types.h"
#include "src/persistence/vhdx_reader.h"
#include "src/storage/backing_store.h"
#include "test_framework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::persistence;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

class CreateSandbox final {
public:
    CreateSandbox() {
        static std::atomic<std::uint64_t> sequence{};
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-m3-create-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(root_);
    }
    ~CreateSandbox() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }
    const std::filesystem::path& root() const noexcept { return root_; }
private:
    std::filesystem::path root_;
};

GenerationZeroCreateRequest create_request(
    const std::filesystem::path& root) {
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
    request.operation_id = 2701;
    request.created_time_100ns = 123456;
    request.vhdx_limits = {4096, 4096, kVhdxDataBlockBytes};
    request.recovery.limits = {
        64 * kMiB, 64 * kMiB, 8192,
        64 * kIndexNodeBytes, 64,
        64 * kVhdxBatPageBytes, 64,
        2 * kMiB, 2 * kMiB};
    return request;
}

std::vector<std::byte> read_all(const std::filesystem::path& path) {
    const auto length = std::filesystem::file_size(path);
    std::vector<std::byte> bytes(static_cast<std::size_t>(length));
    std::ifstream stream(path, std::ios::binary);
    stream.read(
        reinterpret_cast<char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

TEST_CASE(generation_zero_create_reopens_only_through_recovery) {
    CreateSandbox sandbox;
    auto request = create_request(sandbox.root());

    auto created = create_generation_zero_image(
        OperationContext{}, request);

    REQUIRE(created.ok());
    REQUIRE_EQ(created.value().checkpoint_generation, std::uint64_t{0});
    REQUIRE_EQ(created.value().base_generation, std::uint64_t{0});
    REQUIRE_EQ(created.value().selected_slot, std::uint8_t{0});
    REQUIRE(created.value().backing_view.store != nullptr);
    REQUIRE(std::filesystem::exists(sandbox.root() / L"disk.rdm"));
    REQUIRE(std::filesystem::exists(sandbox.root() / L"base-0000.vhdx"));
    REQUIRE(std::filesystem::exists(sandbox.root() / L"journal-e0001.rdj"));
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"base.create.tmp"));
    REQUIRE_EQ(
        std::filesystem::file_size(sandbox.root() / L"disk.rdm"),
        static_cast<std::uintmax_t>(kManifestFileBytes));

    const auto manifest = read_all(sandbox.root() / L"disk.rdm");
    REQUIRE_EQ(manifest.size(), kManifestFileBytes);
    REQUIRE(std::all_of(
        manifest.begin() + static_cast<std::ptrdiff_t>(kManifestSlotBytes),
        manifest.end(),
        [](const std::byte byte) { return byte == std::byte{}; }));

    std::array<std::byte, kM2BlockSize> bytes{};
    auto first = created.value().backing_view.store->load_block(
        OperationContext{}, 0, bytes);
    REQUIRE(first.ok());
    REQUIRE_EQ(first.value().kind, BlockLoadKind::AllZero);
    const auto last_index =
        (request.geometry.virtual_bytes - 1) / kM2BlockSize;
    auto last = created.value().backing_view.store->load_block(
        OperationContext{}, last_index, bytes);
    REQUIRE(last.ok());
    REQUIRE_EQ(last.value().kind, BlockLoadKind::AllZero);
}

TEST_CASE(generation_zero_retry_never_overwrites_published_bundle) {
    CreateSandbox sandbox;
    auto request = create_request(sandbox.root());
    auto first = create_generation_zero_image(OperationContext{}, request);
    REQUIRE(first.ok());
    const auto manifest_before = read_all(sandbox.root() / L"disk.rdm");
    const auto base_before = read_all(sandbox.root() / L"base-0000.vhdx");
    const auto journal_before = read_all(sandbox.root() / L"journal-e0001.rdj");

    auto retry = create_generation_zero_image(OperationContext{}, request);

    REQUIRE(!retry.ok());
    REQUIRE_EQ(read_all(sandbox.root() / L"disk.rdm"), manifest_before);
    REQUIRE_EQ(read_all(sandbox.root() / L"base-0000.vhdx"), base_before);
    REQUIRE_EQ(read_all(sandbox.root() / L"journal-e0001.rdj"), journal_before);
}

TEST_CASE(generation_zero_invalid_request_creates_no_artifacts) {
    CreateSandbox sandbox;
    auto request = create_request(sandbox.root());
    request.geometry.virtual_bytes = 0;

    auto created = create_generation_zero_image(
        OperationContext{}, request);

    REQUIRE(!created.ok());
    REQUIRE(std::filesystem::is_empty(sandbox.root()));
}

TEST_CASE(generation_zero_rejects_existing_manifest_without_mutation) {
    CreateSandbox sandbox;
    auto request = create_request(sandbox.root());
    const auto manifest = sandbox.root() / L"disk.rdm";
    const std::array<char, 7> sentinel{'e', 'x', 'i', 's', 't', 's', '!'};
    {
        std::ofstream stream(manifest, std::ios::binary);
        stream.write(sentinel.data(), sentinel.size());
    }

    auto created = create_generation_zero_image(
        OperationContext{}, request);

    REQUIRE(!created.ok());
    REQUIRE_EQ(std::filesystem::file_size(manifest), sentinel.size());
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"base-0000.vhdx"));
    REQUIRE(!std::filesystem::exists(sandbox.root() / L"journal-e0001.rdj"));
}

TEST_CASE(generation_zero_rejects_path_escape_before_creating_artifacts) {
    CreateSandbox sandbox;
    auto request = create_request(sandbox.root());
    request.journal_relative_name = "../escape.rdj";

    auto created = create_generation_zero_image(
        OperationContext{}, request);

    REQUIRE(!created.ok());
    REQUIRE(std::filesystem::is_empty(sandbox.root()));
    REQUIRE(!std::filesystem::exists(
        sandbox.root().parent_path() / L"escape.rdj"));
}

TEST_CASE(generation_zero_low_space_gate_precedes_manifest_claim) {
    CreateSandbox sandbox;
    auto request = create_request(sandbox.root());
    request.free_space_reserve_bytes =
        (std::numeric_limits<std::uint64_t>::max)();

    auto created = create_generation_zero_image(
        OperationContext{}, request);

    REQUIRE(!created.ok());
    REQUIRE_EQ(
        created.error().code,
        static_cast<std::uint32_t>(PersistenceErrorCode::DiskFull));
    REQUIRE(std::filesystem::is_empty(sandbox.root()));
}

TEST_CASE(generation_zero_concurrent_creators_have_one_complete_winner) {
    CreateSandbox sandbox;
    const auto request = create_request(sandbox.root());
    constexpr std::size_t contenders = 6;
    std::array<bool, contenders> success{};
    std::array<bool, contenders> expected_collision{};
    std::array<std::thread, contenders> threads{};
    std::atomic<bool> start{};
    for (std::size_t index = 0; index < contenders; ++index) {
        threads[index] = std::thread([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            auto result = create_generation_zero_image(
                OperationContext{}, request);
            success[index] = result.ok();
            if (!result.ok()) {
                expected_collision[index] =
                    result.error().domain == ErrorDomain::storage &&
                    result.error().code == static_cast<std::uint32_t>(
                        ImagePathErrorCode::ImageInUse);
            }
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();

    REQUIRE_EQ(
        std::count(success.begin(), success.end(), true),
        std::ptrdiff_t{1});
    REQUIRE_EQ(
        std::count(expected_collision.begin(), expected_collision.end(), true),
        static_cast<std::ptrdiff_t>(contenders - 1));
    auto recovered = recover_complete_image(
        OperationContext{}, sandbox.root(), "disk.rdm", request.recovery);
    REQUIRE(recovered.ok());
    REQUIRE_EQ(recovered.image->checkpoint_generation, std::uint64_t{0});
}

}  // namespace
