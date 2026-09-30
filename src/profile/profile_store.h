#pragma once

#include "src/common/result.h"
#include "src/profile/profile_types.h"
#include "src/product/uuid.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace myrd::profile {

enum class ProfileStoreError : std::uint32_t {
    invalid_root = 0x4901,
    invalid_profile_id,
    path_not_found,
    unsafe_reparse,
    unsafe_acl,
    io_failure,
    invalid_file_size,
    commit_indeterminate,
    current_corrupt,
    previous_corrupt,
    profile_id_mismatch,
    revision_conflict,
    validation_failed,
    repair_precondition_failed,
    revision_exhausted,
};

struct AtomicTempFile final {
    std::uint64_t value{};

    friend bool operator==(const AtomicTempFile&,
                           const AtomicTempFile&) noexcept = default;
};

class IAtomicFileDirectory {
public:
    virtual ~IAtomicFileDirectory() noexcept = default;

    virtual Result<std::vector<std::byte>> read_file(
        std::wstring_view name) = 0;
    // A failed creation must not leave a persistent temp file or live token.
    virtual Result<AtomicTempFile> create_unique_temp(
        std::wstring_view purpose) = 0;
    virtual Result<void> write_temp(
        AtomicTempFile token, std::span<const std::byte> bytes) = 0;
    virtual Result<void> flush_temp(AtomicTempFile token) = 0;
    virtual Result<std::vector<std::byte>> read_temp(
        AtomicTempFile token) = 0;
    virtual Result<void> publish_temp(
        AtomicTempFile token,
        std::wstring_view destination,
        bool replace_existing) = 0;
    virtual Result<void> remove_file(std::wstring_view name) = 0;
    virtual Result<std::vector<std::wstring>> list_files() = 0;
    virtual Result<void> flush_directory() = 0;
    virtual void discard_temp(AtomicTempFile token) noexcept = 0;
};

struct ManagedAtomicDirectory final {
    std::wstring path;
    std::unique_ptr<IAtomicFileDirectory> directory;
};

Result<ManagedAtomicDirectory> open_managed_atomic_directory(
    std::wstring_view trusted_program_data_root,
    std::span<const std::wstring_view> managed_components);

Result<void> publish_atomically_with_previous(
    IAtomicFileDirectory& directory,
    std::wstring_view current_name,
    std::wstring_view previous_name,
    std::span<const std::byte> new_bytes);

Result<void> publish_current_preserving_previous(
    IAtomicFileDirectory& directory,
    std::wstring_view current_name,
    std::span<const std::byte> new_bytes);

struct ProfileStorePaths final {
    std::wstring product_root;
    std::wstring profiles_root;
    std::wstring profile_root;
    std::wstring current_profile;
    std::wstring previous_profile;
    std::wstring runtime_lease;
};

Result<ProfileStorePaths> derive_profile_store_paths(
    std::wstring_view trusted_program_data_root,
    const product::Uuid& profile_id);

// Creates the management-owned profile directory after validating the
// supplied root and profile identifier. Existing directories are accepted
// only when open_existing subsequently verifies their type, identity, and ACL.
Result<void> create_profile_directory(
    std::wstring_view trusted_program_data_root,
    const product::Uuid& profile_id);

class ProfileDirectoryCapability final : public IAtomicFileDirectory {
public:
    static Result<ProfileDirectoryCapability> open_existing(
        std::wstring_view trusted_program_data_root,
        const product::Uuid& profile_id);

    ProfileDirectoryCapability(ProfileDirectoryCapability&&) noexcept;
    ProfileDirectoryCapability& operator=(
        ProfileDirectoryCapability&&) noexcept;
    ~ProfileDirectoryCapability() noexcept;

    ProfileDirectoryCapability(const ProfileDirectoryCapability&) = delete;
    ProfileDirectoryCapability& operator=(
        const ProfileDirectoryCapability&) = delete;

    const ProfileStorePaths& paths() const noexcept;

    Result<std::vector<std::byte>> read_file(
        std::wstring_view name) override;
    Result<AtomicTempFile> create_unique_temp(
        std::wstring_view purpose) override;
    Result<void> write_temp(
        AtomicTempFile token, std::span<const std::byte> bytes) override;
    Result<void> flush_temp(AtomicTempFile token) override;
    Result<std::vector<std::byte>> read_temp(
        AtomicTempFile token) override;
    Result<void> publish_temp(
        AtomicTempFile token,
        std::wstring_view destination,
        bool replace_existing) override;
    Result<void> remove_file(std::wstring_view name) override;
    Result<std::vector<std::wstring>> list_files() override;
    Result<void> flush_directory() override;
    void discard_temp(AtomicTempFile token) noexcept override;

private:
    struct Impl;
    explicit ProfileDirectoryCapability(std::unique_ptr<Impl> impl) noexcept;
    friend Result<ManagedAtomicDirectory> open_managed_atomic_directory(
        std::wstring_view,
        std::span<const std::wstring_view>);

    std::unique_ptr<Impl> impl_;
};

class IProfileRepairBindingVerifier {
public:
    virtual ~IProfileRepairBindingVerifier() noexcept = default;
    virtual Result<void> verify_repair_binding(
        const ProfileRecord& previous) = 0;
};

class ProfileStore final {
public:
    static Result<ProfileStore> open_existing(
        std::wstring_view trusted_program_data_root,
        const product::Uuid& profile_id);
    static Result<ProfileStore> from_directory_capability(
        std::unique_ptr<IAtomicFileDirectory> directory,
        ProfileStorePaths paths,
        const product::Uuid& profile_id);

    ProfileStore(ProfileStore&&) noexcept;
    ProfileStore& operator=(ProfileStore&&) noexcept;
    ~ProfileStore() noexcept;

    ProfileStore(const ProfileStore&) = delete;
    ProfileStore& operator=(const ProfileStore&) = delete;

    const ProfileStorePaths& paths() const noexcept;
    Result<ProfileRecord> load_current();
    Result<ProfileRecord> load_previous();
    Result<void> write_next(
        std::uint64_t expected_revision,
        const ProfileRecord& proposed,
        bool profile_is_running);
    Result<void> write_rebind(
        std::uint64_t expected_revision,
        const ProfileRecord& proposed);
    Result<std::uint64_t> claimed_current_revision();
    Result<void> repair_from_previous(
        std::uint64_t expected_claimed_current_revision,
        IProfileRepairBindingVerifier& verifier);

private:
    struct Impl;
    explicit ProfileStore(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

} // namespace myrd::profile
