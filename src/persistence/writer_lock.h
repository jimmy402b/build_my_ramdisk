#pragma once

#include "src/common/result.h"
#include "src/common/operation_context.h"
#include "src/common/unique_handle.h"
#include "src/persistence/file_identity.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace myrd::persistence {

class ValidatedBundleFile;
class BundleWriterLock;

#if defined(MYRD_ENABLE_TEST_SEAMS)
namespace test {

[[nodiscard]] bool quarantine_empty_native_rename_for_writer_for_test(
    BundleWriterLock& writer);
[[nodiscard]] std::pair<std::uint64_t, std::uint64_t>
native_rename_writer_counts_for_test(const BundleWriterLock& writer);

} // namespace test
#endif

namespace detail {

struct WriterAuthorityState final {
    mutable std::mutex mutex;
    std::condition_variable native_rename_condition;
    UniqueHandle root_handle;
    UniqueHandle manifest_handle;
    UniqueHandle close_event;
    WindowsFileIdentity root_identity{};
    WindowsFileIdentity manifest_identity{};
    std::filesystem::path root_path;
    std::wstring root_final_path;
    std::wstring manifest_final_path;
    std::string manifest_relative_name;
    std::wstring registry_key;
    OVERLAPPED lock_overlapped{};
    bool registry_held{};
    bool byte_lock_held{};
    bool active{};
    bool close_detached{};
    std::uint64_t active_native_renames{};
    std::uint64_t quarantined_native_renames{};
};

[[nodiscard]] Result<void> verify_writer_state_locked(
    const WriterAuthorityState& state,
    bool allow_close_requested = false);

void complete_native_rename_locked(
    WriterAuthorityState& state,
    bool was_quarantined) noexcept;

} // namespace detail

class BundleWriterLock final {
public:
    BundleWriterLock(BundleWriterLock&& other) noexcept;
    BundleWriterLock& operator=(BundleWriterLock&& other) noexcept;
    ~BundleWriterLock() noexcept;

    BundleWriterLock(const BundleWriterLock&) = delete;
    BundleWriterLock& operator=(const BundleWriterLock&) = delete;

    [[nodiscard]] Result<void> verify() const;
    [[nodiscard]] Result<void> close();

    [[nodiscard]] WindowsFileIdentity manifest_identity() const;

private:
    std::atomic<std::shared_ptr<detail::WriterAuthorityState>> state_;

    [[nodiscard]] std::shared_ptr<detail::WriterAuthorityState>
    snapshot_state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    explicit BundleWriterLock(
        std::shared_ptr<detail::WriterAuthorityState> state) noexcept
        : state_(std::move(state)) {}

    friend Result<BundleWriterLock> acquire_bundle_writer_lock(
        const std::filesystem::path&, std::string_view);
    friend Result<ValidatedBundleFile> open_bundle_file_for_mutation(
        BundleWriterLock&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
    friend Result<ValidatedBundleFile> open_locked_manifest_for_mutation(
        BundleWriterLock&);
    friend Result<void> truncate_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&, std::uint64_t);
    friend Result<void> rename_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&, std::string_view);
    friend Result<void> rename_validated_bundle_file(
        const OperationContext&,
        BundleWriterLock&,
        ValidatedBundleFile&,
        std::string_view);
    friend Result<void> delete_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&);
#if defined(MYRD_ENABLE_TEST_SEAMS)
    friend bool test::quarantine_empty_native_rename_for_writer_for_test(
        BundleWriterLock&);
    friend std::pair<std::uint64_t, std::uint64_t>
    test::native_rename_writer_counts_for_test(const BundleWriterLock&);
#endif
};

[[nodiscard]] Result<BundleWriterLock> acquire_bundle_writer_lock(
    const std::filesystem::path& bundle_root,
    std::string_view manifest_relative_name);

} // namespace myrd::persistence
