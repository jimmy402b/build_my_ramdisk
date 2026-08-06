#pragma once

#include "src/common/result.h"
#include "src/common/unique_handle.h"
#include "src/persistence/file_identity.h"
#include "src/persistence/file_io.h"
#include "src/persistence/persistence_types.h"

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
class BackingViewBindingState;
#if defined(MYRD_ENABLE_TEST_SEAMS)
class INativeRenameBackend;
class INativeSparseBackend;
#endif
}

class BundleWriterLock;
class BackingDeleteGuard;
class OpenedBackingViewFile;
class CompositeBackingStore;
class ValidatedPersistenceFile;

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
    std::shared_ptr<const detail::BackingViewBindingState>
        backing_view_binding_{};
    bool deleted_{};
    bool mutable_access_{};
    bool cleanup_authorized_{};
    bool manifest_identity_bound_{};
    bool poisoned_{};

    ValidatedBundleFile(
        UniqueHandle root_handle,
        UniqueHandle file_handle,
        WindowsFileIdentity root_identity,
        WindowsFileIdentity identity,
        std::wstring root_final_path,
        std::wstring final_path,
        std::string relative_name,
        bool mutable_access,
        bool cleanup_authorized,
        bool manifest_identity_bound = false) noexcept;

    friend Result<ValidatedBundleFile> open_bundle_file_for_inspection(
        const std::filesystem::path&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
    friend Result<ValidatedBundleFile> open_bundle_file_for_mutation(
        BundleWriterLock&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
    friend Result<ValidatedBundleFile> open_bundle_file_for_offline_validation(
        const std::filesystem::path&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
    friend Result<OpenedBackingViewFile> open_bundle_file_for_backing_view(
        const std::filesystem::path&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
    friend Result<ValidatedBundleFile> open_bundle_file_for_immutable_anchor(
        BundleWriterLock&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
    friend Result<ValidatedBundleFile> create_bundle_file_for_mutation(
        BundleWriterLock&,
        std::string_view);
    friend Result<ValidatedBundleFile> open_locked_manifest_for_mutation(
        BundleWriterLock&);
    friend class ValidatedPersistenceFile;
    friend Result<std::unique_ptr<ValidatedPersistenceFile>>
        open_validated_persistence_file(ValidatedBundleFile&&);
    friend Result<void> truncate_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&, std::uint64_t);
    friend Result<void> mark_validated_bundle_file_sparse(
        const OperationContext&, BundleWriterLock&, ValidatedBundleFile&);
    friend Result<void> rename_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&, std::string_view);
    friend Result<void> rename_validated_bundle_file(
        const OperationContext&,
        BundleWriterLock&,
        ValidatedBundleFile&,
        std::string_view);
    friend Result<void> delete_validated_bundle_file(
        BundleWriterLock&, ValidatedBundleFile&);
    friend Result<BundleWriterLock> acquire_bundle_writer_lock(
        const std::filesystem::path&, std::string_view);
    friend Result<ValidatedBundleFile>
        detail_create_manifest_for_writer_acquire(
            const std::filesystem::path&, std::string_view);
};

// Opaque, move-only Windows namespace-retention anchor. Only the backing-view
// factory can create one, and no public API exposes or closes its HANDLE.
// CompositeBackingStore consumes and owns these anchors for its full lifetime.
class BackingDeleteGuard final {
public:
    BackingDeleteGuard(const BackingDeleteGuard&) = delete;
    BackingDeleteGuard& operator=(const BackingDeleteGuard&) = delete;
    BackingDeleteGuard(BackingDeleteGuard&&) noexcept = default;
    BackingDeleteGuard& operator=(BackingDeleteGuard&&) noexcept = default;
    ~BackingDeleteGuard() noexcept = default;

    [[nodiscard]] const WindowsFileIdentity& identity() const noexcept {
        return bound_identity();
    }

private:
    std::shared_ptr<const detail::BackingViewBindingState> binding_{};

    explicit BackingDeleteGuard(
        std::shared_ptr<const detail::BackingViewBindingState>
            binding) noexcept
        : binding_(std::move(binding)) {}

    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] const WindowsFileIdentity& bound_identity() const noexcept;

    friend class CompositeBackingStore;
    friend Result<OpenedBackingViewFile> open_bundle_file_for_backing_view(
        const std::filesystem::path&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
};

// The reader capability and its independent no-DELETE anchor originate from
// duplicate handles to the same validated file object. Callers may hand the
// reader to a codec without giving that codec authority to release the guard.
class OpenedBackingViewFile final {
public:
    OpenedBackingViewFile(const OpenedBackingViewFile&) = delete;
    OpenedBackingViewFile& operator=(const OpenedBackingViewFile&) = delete;
    OpenedBackingViewFile(OpenedBackingViewFile&&) noexcept = default;
    OpenedBackingViewFile& operator=(OpenedBackingViewFile&&) noexcept = default;
    ~OpenedBackingViewFile() noexcept = default;

    [[nodiscard]] const WindowsFileIdentity& identity() const noexcept {
        return guard_.identity();
    }
    [[nodiscard]] ValidatedBundleFile take_file() noexcept {
        return std::move(file_);
    }
    [[nodiscard]] BackingDeleteGuard take_delete_guard() noexcept {
        return std::move(guard_);
    }

private:
    ValidatedBundleFile file_;
    BackingDeleteGuard guard_;

    OpenedBackingViewFile(
        ValidatedBundleFile file,
        BackingDeleteGuard guard) noexcept
        : file_(std::move(file)), guard_(std::move(guard)) {}

    friend Result<OpenedBackingViewFile> open_bundle_file_for_backing_view(
        const std::filesystem::path&,
        std::string_view,
        std::optional<WindowsFileIdentity>);
};

// Internal bootstrap capability used only to atomically claim a previously
// absent manifest name before a BundleWriterLock exists. The returned handle
// stays open without delete sharing while writer acquisition revalidates the
// same file identity, preventing a namespace swap in between.
[[nodiscard]] Result<ValidatedBundleFile>
detail_create_manifest_for_writer_acquire(
    const std::filesystem::path& bundle_root,
    std::string_view manifest_relative_name);

// Owns both the validated capability and a DuplicateHandle-backed Task 11
// exact-I/O view. Codecs can consume this as IPersistenceFile without ever
// seeing a path or raw HANDLE. Mutation remains bound to one writer lifetime.
class ValidatedPersistenceFile final : public IPersistenceFile {
public:
    ~ValidatedPersistenceFile() noexcept override;

    ValidatedPersistenceFile(const ValidatedPersistenceFile&) = delete;
    ValidatedPersistenceFile& operator=(const ValidatedPersistenceFile&) = delete;
    ValidatedPersistenceFile(ValidatedPersistenceFile&&) = delete;
    ValidatedPersistenceFile& operator=(ValidatedPersistenceFile&&) = delete;

    Result<void> read_exact(
        std::uint64_t offset,
        std::span<std::byte> destination) override;
    Result<void> write_exact(
        std::uint64_t offset,
        std::span<const std::byte> source) override;
    Result<void> flush() override;
    Result<std::uint64_t> length() const override;
    Result<void> truncate(std::uint64_t new_length) override;

    Result<void> read_exact(
        const OperationContext& context,
        std::uint64_t offset,
        std::span<std::byte> destination) override;
    Result<void> write_exact(
        const OperationContext& context,
        std::uint64_t offset,
        std::span<const std::byte> source) override;
    Result<void> flush(const OperationContext& context) override;
    Result<std::uint64_t> length(
        const OperationContext& context) const override;
    Result<void> truncate(
        const OperationContext& context,
        std::uint64_t new_length) override;
    Result<void> close() override;

    [[nodiscard]] WindowsFileIdentity observed_identity() const;
    [[nodiscard]] std::wstring observed_final_path() const;
    [[nodiscard]] Result<WindowsFileIdentity>
    verify_mutation_capability();
    [[nodiscard]] Result<ValidatedBundleFile>
    release_validated_bundle_file();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    ValidatedPersistenceFile(
        ValidatedBundleFile capability,
        std::unique_ptr<IPersistenceFile> file);

    [[nodiscard]] Result<std::shared_ptr<detail::WriterAuthorityState>>
    writer_authority_for_mutation(const wchar_t* operation) const;
    [[nodiscard]] Result<void> refresh_identity_after_mutation(
        const std::shared_ptr<detail::WriterAuthorityState>& authority);
    [[nodiscard]] std::shared_ptr<
        const detail::BackingViewBindingState>
    backing_view_binding() const noexcept;

    friend Result<std::unique_ptr<ValidatedPersistenceFile>>
        open_validated_persistence_file(ValidatedBundleFile&&);
    friend class JournalReader;
    friend class VhdxReader;
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

// Runtime backing views may coexist with checkpoint appends but never with
// namespace deletion. The kernel sharing contract is the final retention
// backstop even if higher-level reference accounting is mis-composed.
[[nodiscard]] Result<OpenedBackingViewFile>
open_bundle_file_for_backing_view(
    const std::filesystem::path& bundle_root,
    std::string_view relative_name,
    std::optional<WindowsFileIdentity> expected_identity = std::nullopt);

// Read pin for offline validation: denies content writes while allowing native
// read-only VHDX validation handles to coexist. The caller releases this pin
// before reacquiring the same identity for write-through publication.
[[nodiscard]] Result<ValidatedBundleFile>
open_bundle_file_for_offline_validation(
    const std::filesystem::path& bundle_root,
    std::string_view relative_name,
    std::optional<WindowsFileIdentity> expected_identity = std::nullopt);

// Final immutable anchor denies write/delete sharing. Exact identity catches
// any namespace swap between rename and this final reopen.
[[nodiscard]] Result<ValidatedBundleFile>
open_bundle_file_for_immutable_anchor(
    BundleWriterLock& writer_lock,
    std::string_view relative_name,
    std::optional<WindowsFileIdentity> expected_identity = std::nullopt);

[[nodiscard]] Result<ValidatedBundleFile>
create_bundle_file_for_mutation(
    BundleWriterLock& writer_lock,
    std::string_view relative_name);

[[nodiscard]] Result<ValidatedBundleFile>
open_locked_manifest_for_mutation(BundleWriterLock& writer_lock);

[[nodiscard]] Result<std::unique_ptr<ValidatedPersistenceFile>>
open_validated_persistence_file(ValidatedBundleFile&& capability);

[[nodiscard]] Result<void> truncate_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    std::uint64_t new_length);

[[nodiscard]] Result<void> mark_validated_bundle_file_sparse(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file);

[[nodiscard]] Result<void> rename_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    std::string_view new_relative_name);

[[nodiscard]] Result<void> rename_validated_bundle_file(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    std::string_view new_relative_name);

[[nodiscard]] Result<void> write_through_rename_validated_bundle_file(
    const OperationContext& context,
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

#if defined(MYRD_ENABLE_TEST_SEAMS)
namespace test {

[[nodiscard]] Result<void> mark_validated_bundle_file_sparse_with_backend(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    detail::INativeSparseBackend& backend);

[[nodiscard]] Result<void>
write_through_rename_validated_bundle_file_with_backend(
    const OperationContext& context,
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    std::string_view new_relative_name,
    detail::INativeRenameBackend& backend);

}  // namespace test
#endif

} // namespace myrd::persistence
