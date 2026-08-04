#pragma once

#include "src/common/result.h"
#include "src/common/unique_handle.h"
#include "src/persistence/file_identity.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace myrd::persistence {

// Task 12 owns this deliberately narrow error surface. Task 13 will migrate
// these stable meanings into the full persistence error context without
// changing the numeric values or collapsing Missing into IdentityMismatch.
enum class ImagePathErrorCode : std::uint32_t {
    InvalidName = 0x3101,
    PathEscape = 0x3102,
    ReparsePoint = 0x3103,
    HardLink = 0x3104,
    Missing = 0x3105,
    IdentityMismatch = 0x3106,
    ImageInUse = 0x3107,
    WriterLockInvalid = 0x3108,
    AccessDenied = 0x3109,
    IoError = 0x310A,
};

struct ImageComponent final {
    std::string utf8_name;
    std::wstring wide_name;
};

[[nodiscard]] Result<ImageComponent>
validate_image_component(std::string_view utf8_name);

namespace detail {
struct WriterAuthorityState;
}

class BundleWriterLock;

// An unforgeable, handle-anchored observation of one direct bundle child.
// final_path() is diagnostic only: later codecs must consume/adapt the held
// handle instead of reopening this string and reintroducing a TOCTOU window.
class ValidatedBundleFile final {
public:
    ValidatedBundleFile(ValidatedBundleFile&&) noexcept = default;
    ValidatedBundleFile& operator=(ValidatedBundleFile&&) noexcept = default;
    ~ValidatedBundleFile() noexcept = default;

    ValidatedBundleFile(const ValidatedBundleFile&) = delete;
    ValidatedBundleFile& operator=(const ValidatedBundleFile&) = delete;

    [[nodiscard]] const WindowsFileIdentity& identity() const noexcept {
        return identity_;
    }
    [[nodiscard]] const std::wstring& final_path() const noexcept {
        return final_path_;
    }
    [[nodiscard]] const std::string& relative_name() const noexcept {
        return relative_name_;
    }
    [[nodiscard]] bool writer_authorized() const noexcept;

private:
    UniqueHandle root_handle_;
    UniqueHandle file_handle_;
    WindowsFileIdentity root_identity_{};
    WindowsFileIdentity identity_{};
    std::wstring root_final_path_;
    std::wstring final_path_;
    std::string relative_name_;
    std::weak_ptr<detail::WriterAuthorityState> writer_authority_;
    bool deleted_{};

    ValidatedBundleFile(
        UniqueHandle root_handle,
        UniqueHandle file_handle,
        WindowsFileIdentity root_identity,
        WindowsFileIdentity identity,
        std::wstring root_final_path,
        std::wstring final_path,
        std::string relative_name) noexcept;

    friend Result<ValidatedBundleFile> open_bundle_file_for_inspection(
        const std::filesystem::path&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
    friend Result<ValidatedBundleFile> open_bundle_file_for_mutation(
        BundleWriterLock&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
    friend Result<void> truncate_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&, std::uint64_t);
    friend Result<void> rename_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&, std::string_view);
    friend Result<void> delete_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&);
    friend Result<BundleWriterLock> acquire_bundle_writer_lock(
        const std::filesystem::path&, std::string_view);
};

[[nodiscard]] Result<std::wstring>
normalized_final_path(HANDLE handle);

[[nodiscard]] Result<bool>
is_direct_bundle_child(HANDLE bundle_root, HANDLE target);

[[nodiscard]] Result<ValidatedBundleFile>
open_bundle_file_for_inspection(
    const std::filesystem::path& bundle_root,
    std::string_view relative_name,
    std::optional<WindowsFileIdentity> expected_identity = std::nullopt);

[[nodiscard]] Result<ValidatedBundleFile>
open_bundle_file_for_mutation(
    BundleWriterLock& writer_lock,
    std::string_view relative_name,
    std::optional<WindowsFileIdentity> expected_identity = std::nullopt);

[[nodiscard]] Result<void> truncate_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    std::uint64_t new_length);

[[nodiscard]] Result<void> rename_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    std::string_view new_relative_name);

[[nodiscard]] Result<void> delete_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file);

// These mutation helpers enforce path, identity, hard-link and writer-lifetime
// authority only. Task 36 remains responsible for proving that no valid
// manifest slot, BackingView, in-flight load or reconciliation still references
// a candidate before it invokes cleanup.

template <class T>
[[nodiscard]] bool is_image_path_error(
    const Result<T>& result,
    const ImagePathErrorCode code) noexcept {
    return result.has_error() &&
        result.error().domain == ErrorDomain::storage &&
        result.error().code == static_cast<std::uint32_t>(code);
}

} // namespace myrd::persistence
