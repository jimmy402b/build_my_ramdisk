#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "src/persistence/composite_backing_store.h"
#include "src/persistence/image_creator.h"
#include "src/persistence/recovered_store_adapter.h"
#include "src/persistence/vhdx_reader.h"
#include "src/storage/dynamic_block_store_internal.h"
#include "test_framework.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::persistence;

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

class LazySandbox final {
public:
    LazySandbox() {
        static std::atomic<std::uint64_t> sequence{};
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-m3-lazy-" + std::to_wstring(GetCurrentProcessId()) +
             L"-" + std::to_wstring(sequence.fetch_add(1)));
        std::filesystem::create_directories(root_);
    }
    ~LazySandbox() noexcept {
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
    request.base_relative_name = "base-0000.vhdx";
    request.journal_relative_name = "journal-e0001.rdj";
    request.geometry = {8 * kMiB, 512, 4096};
    request.image_uuid.bytes[0] = std::byte{0x51};
    request.base_identity.file_identity.file_write_guid[0] =
        std::byte{0x61};
    request.base_identity.file_identity.data_write_guid[0] =
        std::byte{0x62};
    request.base_identity.virtual_disk_id[0] = std::byte{0x63};
    request.journal_identity.journal_uuid[0] = std::byte{0x71};
    request.operation_id = 2801;
    request.vhdx_limits = {4096, 4096, kVhdxDataBlockBytes};
    request.recovery.limits = {
        64 * kMiB, 64 * kMiB, 8192,
        64 * kIndexNodeBytes, 64,
        64 * kVhdxBatPageBytes, 64,
        2 * kMiB, 2 * kMiB};
    return request;
}

DynamicBlockStoreOptions store_options() {
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

RecoveredImage recover_fresh(const GenerationZeroCreateRequest& request) {
    auto outcome = recover_complete_image(
        OperationContext{}, request.bundle_root,
        request.manifest_relative_name, request.recovery);
    REQUIRE(outcome.ok());
    return std::move(*outcome.image);
}

TEST_CASE(recovered_store_open_is_lazy_and_first_touch_is_cached) {
    LazySandbox sandbox;
    const auto request = create_request(sandbox.root());
    REQUIRE(create_generation_zero_image(
        OperationContext{}, request).ok());
    auto recovered = recover_fresh(request);
    auto composite = std::dynamic_pointer_cast<const CompositeBackingStore>(
        recovered.backing_view.store);
    REQUIRE(composite != nullptr);
    REQUIRE_EQ(composite->stats().load_calls, std::uint64_t{0});
    REQUIRE_EQ(recovered.diagnostics[0].base_payload_read_calls_at_open,
               std::uint64_t{0});

    auto opened = open_recovered_dynamic_store(
        recovered, store_options());

    REQUIRE(opened.ok());
    auto store = opened.move_value();
    REQUIRE_EQ(composite->stats().load_calls, std::uint64_t{0});
    REQUIRE_EQ(store->stats().committed_bytes, std::uint64_t{0});
    std::array<std::byte, kM2BlockSize> bytes{};
    bytes.fill(std::byte{0xa5});
    REQUIRE(store->read(OperationContext{}, 0, bytes).ok());
    REQUIRE(std::all_of(bytes.begin(), bytes.end(), [](const std::byte byte) {
        return byte == std::byte{};
    }));
    const auto after_first = composite->stats();
    REQUIRE_EQ(after_first.load_calls, std::uint64_t{1});
    REQUIRE_EQ(after_first.journal_read_calls, std::uint64_t{1});
    REQUIRE_EQ(after_first.base_read_calls, std::uint64_t{1});
    REQUIRE_EQ(after_first.fallback_calls, std::uint64_t{1});
    REQUIRE_EQ(store->stats().committed_bytes, std::uint64_t{0});

    bytes.fill(std::byte{0xa5});
    REQUIRE(store->read(OperationContext{}, 0, bytes).ok());
    REQUIRE_EQ(composite->stats().load_calls, std::uint64_t{1});
    REQUIRE(store->validate_invariants_for_test().ok());
}

TEST_CASE(recovered_store_concurrent_first_touch_loads_one_backing_block) {
    LazySandbox sandbox;
    const auto request = create_request(sandbox.root());
    REQUIRE(create_generation_zero_image(
        OperationContext{}, request).ok());
    auto recovered = recover_fresh(request);
    auto composite = std::dynamic_pointer_cast<const CompositeBackingStore>(
        recovered.backing_view.store);
    REQUIRE(composite != nullptr);
    auto opened = open_recovered_dynamic_store(recovered, store_options());
    REQUIRE(opened.ok());
    auto store = opened.move_value();
    constexpr std::size_t readers = 32;
    std::vector<std::vector<std::byte>> outputs(
        readers,
        std::vector<std::byte>(
            static_cast<std::size_t>(kM2BlockSize), std::byte{0xa5}));
    std::array<bool, readers> success{};
    std::array<std::thread, readers> threads{};
    std::atomic<bool> start{};
    for (std::size_t index = 0; index < readers; ++index) {
        threads[index] = std::thread([&, index] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            success[index] = store->read(
                OperationContext{}, 0, outputs[index]).ok();
        });
    }
    start.store(true, std::memory_order_release);
    for (auto& thread : threads) thread.join();
    REQUIRE(std::all_of(success.begin(), success.end(), [](const bool value) {
        return value;
    }));
    REQUIRE_EQ(composite->stats().load_calls, std::uint64_t{1});
    for (const auto& output : outputs) {
        REQUIRE(std::all_of(
            output.begin(), output.end(), [](const std::byte byte) {
                return byte == std::byte{};
            }));
    }
}

TEST_CASE(recovered_store_partial_write_materializes_from_selected_view) {
    LazySandbox sandbox;
    const auto request = create_request(sandbox.root());
    REQUIRE(create_generation_zero_image(
        OperationContext{}, request).ok());
    auto recovered = recover_fresh(request);
    auto composite = std::dynamic_pointer_cast<const CompositeBackingStore>(
        recovered.backing_view.store);
    REQUIRE(composite != nullptr);
    auto opened = open_recovered_dynamic_store(recovered, store_options());
    REQUIRE(opened.ok());
    auto store = opened.move_value();
    std::array<std::byte, 512> patch{};
    for (std::size_t index = 0; index < patch.size(); ++index) {
        patch[index] = static_cast<std::byte>(index % 251 + 1);
    }

    REQUIRE(store->write(OperationContext{}, 512, patch).ok());

    REQUIRE_EQ(composite->stats().load_calls, std::uint64_t{1});
    std::array<std::byte, 1536> bytes{};
    REQUIRE(store->read(OperationContext{}, 0, bytes).ok());
    REQUIRE(std::all_of(bytes.begin(), bytes.begin() + 512,
        [](const std::byte byte) { return byte == std::byte{}; }));
    REQUIRE(std::equal(patch.begin(), patch.end(), bytes.begin() + 512));
    REQUIRE(std::all_of(bytes.begin() + 1024, bytes.end(),
        [](const std::byte byte) { return byte == std::byte{}; }));
    REQUIRE_EQ(store->stats().resident_dirty_blocks, std::uint64_t{1});
}

TEST_CASE(recovered_store_adapter_rejects_mismatch_and_same_generation_install) {
    LazySandbox sandbox;
    const auto request = create_request(sandbox.root());
    REQUIRE(create_generation_zero_image(
        OperationContext{}, request).ok());
    auto recovered = recover_fresh(request);
    auto opened = open_recovered_dynamic_store(recovered, store_options());
    REQUIRE(opened.ok());
    auto store = opened.move_value();

    auto same = install_recovered_backing_view(*store, recovered);
    REQUIRE(!same.ok());
    auto mismatched = recover_fresh(request);
    mismatched.geometry.virtual_bytes += 512;
    auto rejected = install_recovered_backing_view(*store, mismatched);
    REQUIRE(!rejected.ok());
    REQUIRE_EQ(rejected.error().domain, ErrorDomain::persistence);
}

}  // namespace
