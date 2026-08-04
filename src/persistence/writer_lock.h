#pragma once

#include "src/common/result.h"
#include "src/common/unique_handle.h"
#include "src/persistence/file_identity.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace myrd::persistence {

class ValidatedBundleFile;

namespace detail {

struct WriterAuthorityState final {
    mutable std::mutex mutex;
    UniqueHandle root_handle;
    UniqueHandle manifest_handle;
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
};

[[nodiscard]] Result<void> verify_writer_state_locked(
    const WriterAuthorityState& state);

} // namespace detail

class BundleWriterLock final {
public:
    BundleWriterLock(BundleWriterLock&&) noexcept = default;
    BundleWriterLock& operator=(BundleWriterLock&& other) noexcept;
    ~BundleWriterLock() noexcept;

    BundleWriterLock(const BundleWriterLock&) = delete;
    BundleWriterLock& operator=(const BundleWriterLock&) = delete;

    [[nodiscard]] Result<void> verify() const;
    [[nodiscard]] Result<void> close();

    [[nodiscard]] WindowsFileIdentity manifest_identity() const;

private:
    std::shared_ptr<detail::WriterAuthorityState> state_;

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
    friend Result<void> delete_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&);
};

[[nodiscard]] Result<BundleWriterLock> acquire_bundle_writer_lock(
    const std::filesystem::path& bundle_root,
    std::string_view manifest_relative_name);

} // namespace myrd::persistence
