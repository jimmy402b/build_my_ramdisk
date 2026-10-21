#include "src/common/diagnostic_trace.h"
#include "src/profile/profile_store.h"
#include "src/profile/profile_codec.h"
#include "src/profile/profile_types.h"
#include "src/profile/profile_validation.h"

#define NOMINMAX
#include <Windows.h>
#include <sddl.h>
#include <AclAPI.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <filesystem>
#include <map>
#include <limits>
#include <mutex>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace myrd::profile {

namespace {

template <class T>
Result<T> path_error(ProfileStoreError code, const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(code),
        L"profile_store.paths",
        message});
}

bool is_zero(const product::Uuid& value) noexcept {
    return std::all_of(
        value.bytes().begin(), value.bytes().end(),
        [](std::byte byte) { return byte == std::byte{0}; });
}

std::wstring widen_ascii(std::string_view text) {
    return std::wstring(text.begin(), text.end());
}

class NativeHandle final {
public:
    explicit NativeHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value) {}
    ~NativeHandle() noexcept {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(CloseHandle(value_));
        }
    }
    NativeHandle(NativeHandle&& other) noexcept : value_(other.release()) {}
    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this == &other) return *this;
        NativeHandle old(value_);
        value_ = other.release();
        return *this;
    }
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    HANDLE get() const noexcept { return value_; }
    bool valid() const noexcept {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept {
        NativeHandle old(value_);
        value_ = value;
    }

private:
    HANDLE release() noexcept {
        const auto value = value_;
        value_ = nullptr;
        return value;
    }

    HANDLE value_{};
};

class NativeFindHandle final {
public:
    explicit NativeFindHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value) {}
    ~NativeFindHandle() noexcept {
        if (value_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(FindClose(value_));
        }
    }
    NativeFindHandle(const NativeFindHandle&) = delete;
    NativeFindHandle& operator=(const NativeFindHandle&) = delete;
    HANDLE get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE value_;
};

class CreatedFileGuard final {
public:
    CreatedFileGuard(NativeHandle& handle, const std::wstring& path) noexcept
        : handle_(&handle), path_(&path) {}
    ~CreatedFileGuard() noexcept {
        if (handle_ == nullptr) return;
        handle_->reset();
        static_cast<void>(DeleteFileW(path_->c_str()));
    }
    CreatedFileGuard(const CreatedFileGuard&) = delete;
    CreatedFileGuard& operator=(const CreatedFileGuard&) = delete;

    void release() noexcept { handle_ = nullptr; }

private:
    NativeHandle* handle_;
    const std::wstring* path_;
};

struct LocalFreeDeleter final {
    void operator()(void* value) const noexcept {
        if (value != nullptr) static_cast<void>(LocalFree(value));
    }
};

using LocalMemory = std::unique_ptr<void, LocalFreeDeleter>;

Result<void> store_error(ProfileStoreError code,
                         const wchar_t* operation,
                         const wchar_t* message,
                         std::uint32_t native_code = 0) {
    return Result<void>::failure(Error{
        native_code == 0 ? ErrorDomain::protocol : ErrorDomain::win32,
        static_cast<std::uint32_t>(code), operation, message, native_code});
}

Result<NativeHandle> open_directory(std::wstring_view path, bool writable) {
    const std::wstring terminated(path);
    HANDLE handle = CreateFileW(
        terminated.c_str(),
        FILE_READ_ATTRIBUTES | READ_CONTROL
            | (writable ? GENERIC_READ | GENERIC_WRITE : 0U),
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = GetLastError();
        return Result<NativeHandle>::failure(Error{
            ErrorDomain::win32,
            static_cast<std::uint32_t>(
                error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                    ? ProfileStoreError::path_not_found
                    : ProfileStoreError::io_failure),
            L"profile_store.open_directory",
            L"Failed to open a required profile directory", error});
    }
    return Result<NativeHandle>::success(NativeHandle(handle));
}

template <class T>
Result<T> file_error(ProfileStoreError code,
                     const wchar_t* operation,
                     const wchar_t* message,
                     std::uint32_t native_code = 0) {
    return Result<T>::failure(Error{
        native_code == 0 ? ErrorDomain::protocol : ErrorDomain::win32,
        static_cast<std::uint32_t>(code), operation, message, native_code});
}

Result<void> validate_plain_file(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.file_type",
            L"Failed to inspect a profile file", GetLastError());
    }
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard))) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.file_links",
            L"Failed to inspect profile file links", GetLastError());
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0
        || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || tag.ReparseTag != 0 || standard.NumberOfLinks != 1) {
        return store_error(
            ProfileStoreError::unsafe_reparse,
            L"profile_store.file_type",
            L"A profile file is a directory, reparse point, or hard link");
    }
    if (standard.EndOfFile.QuadPart < 0
        || standard.EndOfFile.QuadPart
            > static_cast<LONGLONG>(kProfileMaximumBytes)) {
        return store_error(
            ProfileStoreError::invalid_file_size,
            L"profile_store.file_size",
            L"A profile file exceeds the bounded profile size");
    }
    return Result<void>::success();
}

Result<std::vector<std::byte>> read_plain_file(HANDLE handle) {
    auto valid = validate_plain_file(handle);
    if (!valid.ok()) {
        return Result<std::vector<std::byte>>::failure(valid.error());
    }
    FILE_STANDARD_INFO standard{};
    if (!GetFileInformationByHandleEx(
            handle, FileStandardInfo, &standard, sizeof(standard))) {
        return file_error<std::vector<std::byte>>(
            ProfileStoreError::io_failure,
            L"profile_store.read_file",
            L"Failed to determine profile file size", GetLastError());
    }
    LARGE_INTEGER start{};
    if (!SetFilePointerEx(handle, start, nullptr, FILE_BEGIN)) {
        return file_error<std::vector<std::byte>>(
            ProfileStoreError::io_failure,
            L"profile_store.read_file",
            L"Failed to seek a profile file", GetLastError());
    }
    std::vector<std::byte> bytes(
        static_cast<std::size_t>(standard.EndOfFile.QuadPart));
    DWORD read = 0;
    const BOOL read_ok = bytes.empty()
        || ReadFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                    &read, nullptr);
    if (!read_ok || read != static_cast<DWORD>(bytes.size())) {
        return file_error<std::vector<std::byte>>(
            ProfileStoreError::io_failure,
            L"profile_store.read_file",
            L"Failed to read a complete profile file",
            read_ok ? ERROR_HANDLE_EOF : GetLastError());
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

std::wstring join_leaf(std::wstring_view directory,
                       std::wstring_view leaf) {
    std::wstring result(directory);
    result.push_back(L'\\');
    result.append(leaf);
    return result;
}

Result<void> validate_directory_type(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO tag{};
    if (!GetFileInformationByHandleEx(
            handle, FileAttributeTagInfo, &tag, sizeof(tag))) {
        const auto error = GetLastError();
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.directory_type",
            L"Failed to inspect a profile directory", error);
    }
    if ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0
        || (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0
        || tag.ReparseTag != 0) {
        return store_error(
            ProfileStoreError::unsafe_reparse,
            L"profile_store.directory_type",
            L"A profile path component is not a plain directory");
    }
    return Result<void>::success();
}

std::wstring normalized_handle_path(HANDLE handle) {
    const DWORD required = GetFinalPathNameByHandleW(
        handle, nullptr, 0, FILE_NAME_OPENED | VOLUME_NAME_DOS);
    if (required == 0 || required > 32768U) return {};
    std::wstring value(required, L'\0');
    const DWORD written = GetFinalPathNameByHandleW(
        handle, value.data(), required,
        FILE_NAME_OPENED | VOLUME_NAME_DOS);
    if (written == 0 || written >= required) return {};
    value.resize(written);
    while (value.size() > 4 && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

std::wstring normalized_expected_path(std::wstring_view path) {
    std::wstring expected(path);
    while (expected.size() > 3
           && (expected.back() == L'\\' || expected.back() == L'/')) {
        expected.pop_back();
    }
    if (!expected.starts_with(L"\\\\?\\")) expected.insert(0, L"\\\\?\\");
    std::transform(expected.begin(), expected.end(), expected.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return expected;
}

Result<void> validate_directory_identity(
    HANDLE handle, std::wstring_view expected_path) {
    const auto actual = normalized_handle_path(handle);
    if (actual.empty()) {
        const auto error = GetLastError();
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.directory_identity",
            L"Failed to resolve a profile directory identity", error);
    }
    if (actual != normalized_expected_path(expected_path)) {
        return store_error(
            ProfileStoreError::unsafe_reparse,
            L"profile_store.directory_identity",
            L"A profile path resolved outside its expected directory");
    }
    return Result<void>::success();
}

Result<void> validate_management_acl(HANDLE handle) {
    PACL dacl = nullptr;
    PSECURITY_DESCRIPTOR raw_descriptor = nullptr;
    const DWORD status = GetSecurityInfo(
        handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &dacl, nullptr, &raw_descriptor);
    if (status != ERROR_SUCCESS) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.directory_acl",
            L"Failed to inspect a profile directory ACL", status);
    }
    LocalMemory descriptor(raw_descriptor);
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    if (!GetSecurityDescriptorControl(
            raw_descriptor, &control, &revision)
        || dacl == nullptr || !IsValidAcl(dacl)
        || dacl->AclRevision != ACL_REVISION
        || (control & SE_DACL_PRESENT) == 0
        || (control & SE_DACL_PROTECTED) == 0) {
        return store_error(
            ProfileStoreError::unsafe_acl,
            L"profile_store.directory_acl",
            L"A profile directory ACL is not protected");
    }

    std::array<std::byte, SECURITY_MAX_SID_SIZE> system_sid{};
    std::array<std::byte, SECURITY_MAX_SID_SIZE> admin_sid{};
    DWORD system_bytes = static_cast<DWORD>(system_sid.size());
    DWORD admin_bytes = static_cast<DWORD>(admin_sid.size());
    if (!CreateWellKnownSid(
            WinLocalSystemSid, nullptr, system_sid.data(), &system_bytes)
        || !CreateWellKnownSid(
            WinBuiltinAdministratorsSid, nullptr,
            admin_sid.data(), &admin_bytes)) {
        const auto error = GetLastError();
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.directory_acl",
            L"Failed to construct a management SID", error);
    }

    ACL_SIZE_INFORMATION size{};
    if (!GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation)) {
        const auto error = GetLastError();
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.directory_acl",
            L"Failed to inspect profile directory ACL entries", error);
    }
    bool system_allowed = false;
    bool admin_allowed = false;
    for (DWORD index = 0; index < size.AceCount; ++index) {
        void* raw_ace = nullptr;
        if (!GetAce(dacl, index, &raw_ace)) {
            const auto error = GetLastError();
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.directory_acl",
                L"Failed to read a profile directory ACL entry", error);
        }
        const auto* header = static_cast<const ACE_HEADER*>(raw_ace);
        constexpr BYTE kExpectedAceFlags =
            OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE;
        if (header->AceType != ACCESS_ALLOWED_ACE_TYPE
            || header->AceFlags != kExpectedAceFlags
            || header->AceSize
                < offsetof(ACCESS_ALLOWED_ACE, SidStart) + sizeof(DWORD)) {
            return store_error(
                ProfileStoreError::unsafe_acl,
                L"profile_store.directory_acl",
                L"A profile directory has an unexpected ACL entry");
        }
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw_ace);
        PSID sid = const_cast<DWORD*>(&ace->SidStart);
        if (!IsValidSid(sid)
            || offsetof(ACCESS_ALLOWED_ACE, SidStart) + GetLengthSid(sid)
                > header->AceSize) {
            return store_error(
                ProfileStoreError::unsafe_acl,
                L"profile_store.directory_acl",
                L"A profile directory has a malformed management SID");
        }
        if ((ace->Mask & FILE_ALL_ACCESS) != FILE_ALL_ACCESS) {
            return store_error(
                ProfileStoreError::unsafe_acl,
                L"profile_store.directory_acl",
                L"A management ACL entry lacks required access");
        }
        if (EqualSid(sid, system_sid.data())) {
            if (system_allowed) {
                return store_error(
                    ProfileStoreError::unsafe_acl,
                    L"profile_store.directory_acl",
                    L"A profile directory has duplicate ACL entries");
            }
            system_allowed = true;
        } else if (EqualSid(sid, admin_sid.data())) {
            if (admin_allowed) {
                return store_error(
                    ProfileStoreError::unsafe_acl,
                    L"profile_store.directory_acl",
                    L"A profile directory has duplicate ACL entries");
            }
            admin_allowed = true;
        } else {
            return store_error(
                ProfileStoreError::unsafe_acl,
                L"profile_store.directory_acl",
                L"A profile directory grants access to an unexpected identity");
        }
    }
    if (!system_allowed || !admin_allowed || size.AceCount != 2) {
        return store_error(
            ProfileStoreError::unsafe_acl,
            L"profile_store.directory_acl",
            L"A profile directory lacks the exact management ACL");
    }
    return Result<void>::success();
}

bool is_safe_leaf_name(std::wstring_view name) noexcept {
    return !name.empty() && name != L"." && name != L".."
        && name.find(L'\0') == std::wstring_view::npos
        && name.find(L'\\') == std::wstring_view::npos
        && name.find(L'/') == std::wstring_view::npos
        && name.find(L':') == std::wstring_view::npos;
}

Result<void> invalid_publication_argument(const wchar_t* message) {
    return store_error(
        ProfileStoreError::io_failure,
        L"profile_store.publish",
        message);
}

Result<void> commit_indeterminate(const Error& cause) {
    return Result<void>::failure(Error{
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(ProfileStoreError::commit_indeterminate),
        L"profile_store.publish",
        L"Atomic profile publication may already be visible",
        cause.native_code});
}

class TempGuard final {
public:
    TempGuard(IAtomicFileDirectory& directory, AtomicTempFile token) noexcept
        : directory_(&directory), token_(token) {}
    ~TempGuard() noexcept {
        if (directory_ != nullptr) directory_->discard_temp(token_);
    }
    TempGuard(const TempGuard&) = delete;
    TempGuard& operator=(const TempGuard&) = delete;

    void release() noexcept { directory_ = nullptr; }

private:
    IAtomicFileDirectory* directory_;
    AtomicTempFile token_;
};

Result<AtomicTempFile> stage_verified_temp(
    IAtomicFileDirectory& directory,
    std::wstring_view purpose,
    std::span<const std::byte> bytes) {
    auto created = directory.create_unique_temp(purpose);
    if (!created.ok()) return created;

    const auto token = created.value();
    TempGuard guard(directory, token);
    auto written = directory.write_temp(token, bytes);
    if (!written.ok()) {
        return Result<AtomicTempFile>::failure(written.error());
    }
    auto flushed = directory.flush_temp(token);
    if (!flushed.ok()) {
        return Result<AtomicTempFile>::failure(flushed.error());
    }
    auto verified = directory.read_temp(token);
    if (!verified.ok()) {
        return Result<AtomicTempFile>::failure(verified.error());
    }
    if (!std::equal(
            verified.value().begin(), verified.value().end(),
            bytes.begin(), bytes.end())) {
        return Result<AtomicTempFile>::failure(Error{
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(ProfileStoreError::io_failure),
            L"profile_store.verify_temp",
            L"A staged profile file did not verify byte-for-byte"});
    }
    guard.release();
    return Result<AtomicTempFile>::success(token);
}

} // namespace

Result<void> publish_atomically_with_previous(
    IAtomicFileDirectory& directory,
    std::wstring_view current_name,
    std::wstring_view previous_name,
    std::span<const std::byte> new_bytes) {
    bool current_publication_attempted = false;
    try {
        if (!is_safe_leaf_name(current_name)
            || !is_safe_leaf_name(previous_name)
            || current_name == previous_name || new_bytes.empty()
            || new_bytes.size() > kProfileMaximumBytes) {
            return invalid_publication_argument(
                L"Atomic publication requires distinct safe leaf names and data");
        }

        auto current = directory.read_file(current_name);
        const bool current_exists = current.ok();
        if (!current_exists
            && current.error().code != static_cast<std::uint32_t>(
                ProfileStoreError::path_not_found)) {
            return Result<void>::failure(current.error());
        }
        if (current_exists
            && (current.value().empty()
                || current.value().size() > kProfileMaximumBytes)) {
            return invalid_publication_argument(
                L"Existing profile bytes are outside the publication bound");
        }

        myrd::diagnostic_trace("atomic_publish: staging next");
        auto next = stage_verified_temp(directory, L"next", new_bytes);
        myrd::diagnostic_trace("atomic_publish: next staged");
        if (!next.ok()) return Result<void>::failure(next.error());
        TempGuard next_guard(directory, next.value());

        if (current_exists) {
            auto previous = stage_verified_temp(
                directory, L"previous", current.value());
            if (!previous.ok()) return Result<void>::failure(previous.error());
            TempGuard previous_guard(directory, previous.value());
            myrd::diagnostic_trace("atomic_publish: publishing previous");
            auto published_previous = directory.publish_temp(
                previous.value(), previous_name, true);
            myrd::diagnostic_trace("atomic_publish: previous published");
            if (!published_previous.ok()) {
                return Result<void>::failure(published_previous.error());
            }
            previous_guard.release();
        }

        current_publication_attempted = true;
        myrd::diagnostic_trace("atomic_publish: publishing current");
        auto published_current = directory.publish_temp(
            next.value(), current_name, current_exists);
        myrd::diagnostic_trace("atomic_publish: current published");
        if (!published_current.ok()) {
            return commit_indeterminate(published_current.error());
        }
        next_guard.release();

        auto directory_flushed = directory.flush_directory();
        if (!directory_flushed.ok()) {
            return commit_indeterminate(directory_flushed.error());
        }
        return Result<void>::success();
    } catch (...) {
        if (current_publication_attempted) {
            return commit_indeterminate(Error{
                ErrorDomain::internal,
                static_cast<std::uint32_t>(ProfileStoreError::io_failure),
                L"profile_store.publish",
                L"Atomic publication backend threw an exception"});
        }
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.publish",
            L"Atomic publication backend threw an exception");
    }
}

Result<void> publish_current_preserving_previous(
    IAtomicFileDirectory& directory,
    std::wstring_view current_name,
    std::span<const std::byte> new_bytes) {
    bool current_publication_attempted = false;
    try {
        if (!is_safe_leaf_name(current_name) || new_bytes.empty()
            || new_bytes.size() > kProfileMaximumBytes) {
            return invalid_publication_argument(
                L"Repair publication requires a safe current leaf and data");
        }
        auto current = directory.read_file(current_name);
        if (!current.ok()) return Result<void>::failure(current.error());

        auto next = stage_verified_temp(directory, L"repair", new_bytes);
        if (!next.ok()) return Result<void>::failure(next.error());
        TempGuard next_guard(directory, next.value());
        current_publication_attempted = true;
        auto published = directory.publish_temp(
            next.value(), current_name, true);
        if (!published.ok()) return commit_indeterminate(published.error());
        next_guard.release();
        auto flushed = directory.flush_directory();
        if (!flushed.ok()) return commit_indeterminate(flushed.error());
        return Result<void>::success();
    } catch (...) {
        if (current_publication_attempted) {
            return commit_indeterminate(Error{
                ErrorDomain::internal,
                static_cast<std::uint32_t>(ProfileStoreError::io_failure),
                L"profile_store.repair_publish",
                L"Repair publication backend threw an exception"});
        }
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.repair_publish",
            L"Repair publication backend threw an exception");
    }
}

struct ProfileDirectoryCapability::Impl final {
    struct TempState final {
        std::wstring leaf;
        std::wstring path;
        NativeHandle handle;
        bool written{};
        bool flushed{};
        bool verified{};
        std::vector<std::byte> verified_bytes;
    };

    ProfileStorePaths paths;
    std::vector<NativeHandle> directories;
    std::mutex mutex;
    std::map<std::uint64_t, TempState> temps;
    std::uint64_t next_token{1};
};

Result<ProfileStorePaths> derive_profile_store_paths(
    std::wstring_view trusted_program_data_root,
    const product::Uuid& profile_id) {
    try {
        if (trusted_program_data_root.empty()
            || trusted_program_data_root.find(L'\0') != std::wstring_view::npos) {
            return path_error<ProfileStorePaths>(
                ProfileStoreError::invalid_root,
                L"The trusted ProgramData root is invalid");
        }
        const std::filesystem::path supplied{
            std::wstring(trusted_program_data_root)};
        if (!supplied.is_absolute()
            || supplied.lexically_normal() != supplied) {
            return path_error<ProfileStorePaths>(
                ProfileStoreError::invalid_root,
                L"The trusted ProgramData root must be absolute and normalized");
        }
        if (is_zero(profile_id)) {
            return path_error<ProfileStorePaths>(
                ProfileStoreError::invalid_profile_id,
                L"The profile identifier must be nonzero");
        }

        const auto product_root = supplied / L"MyRamDisk";
        const auto profiles_root = product_root / L"profiles";
        const auto profile_root = profiles_root
            / widen_ascii(profile_id.to_string());
        return Result<ProfileStorePaths>::success(ProfileStorePaths{
            product_root.wstring(),
            profiles_root.wstring(),
            profile_root.wstring(),
            (profile_root / L"profile.myrd").wstring(),
            (profile_root / L"profile.prev").wstring(),
            (profile_root / L"runtime.lease").wstring()});
    } catch (...) {
        return path_error<ProfileStorePaths>(
            ProfileStoreError::invalid_root,
            L"The trusted ProgramData root cannot be represented safely");
    }
}

Result<void> create_profile_directory(
    std::wstring_view trusted_program_data_root,
    const product::Uuid& profile_id) {
    try {
        auto paths = derive_profile_store_paths(
            trusted_program_data_root, profile_id);
        if (!paths.ok()) return Result<void>::failure(paths.error());

        constexpr std::array<std::wstring_view, 2> components{
            L"MyRamDisk", L"profiles"};
        auto managed = open_managed_atomic_directory(
            trusted_program_data_root, components);
        if (!managed.ok()) return Result<void>::failure(managed.error());

        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)",
                SDDL_REVISION_1, &descriptor, nullptr)) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.create_directory",
                L"Failed to create the profile directory security descriptor",
                GetLastError());
        }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const BOOL created = CreateDirectoryW(
            paths.value().profile_root.c_str(), &attributes);
        const auto native_error = created ? ERROR_SUCCESS : GetLastError();
        LocalFree(descriptor);
        if (!created && native_error != ERROR_ALREADY_EXISTS) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.create_directory",
                L"Failed to create the protected profile directory",
                native_error);
        }

        auto verified = ProfileDirectoryCapability::open_existing(
            trusted_program_data_root, profile_id);
        if (!verified.ok()) return Result<void>::failure(verified.error());
        return Result<void>::success();
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.create_directory",
            L"Failed to allocate protected profile directory state");
    }
}

ProfileDirectoryCapability::ProfileDirectoryCapability(
    std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ProfileDirectoryCapability::ProfileDirectoryCapability(
    ProfileDirectoryCapability&&) noexcept = default;

ProfileDirectoryCapability& ProfileDirectoryCapability::operator=(
    ProfileDirectoryCapability&&) noexcept = default;

ProfileDirectoryCapability::~ProfileDirectoryCapability() noexcept = default;

const ProfileStorePaths& ProfileDirectoryCapability::paths() const noexcept {
    return impl_->paths;
}

Result<std::vector<std::byte>> ProfileDirectoryCapability::read_file(
    std::wstring_view name) {
    try {
        if (!is_safe_leaf_name(name)) {
            return file_error<std::vector<std::byte>>(
                ProfileStoreError::io_failure,
                L"profile_store.read_file",
                L"Profile files must use safe leaf names");
        }
        const auto path = join_leaf(impl_->paths.profile_root, name);
        NativeHandle handle(CreateFileW(
            path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.valid()) {
            const auto error = GetLastError();
            return file_error<std::vector<std::byte>>(
                error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                    ? ProfileStoreError::path_not_found
                    : ProfileStoreError::io_failure,
                L"profile_store.read_file",
                L"Failed to open a profile file", error);
        }
        return read_plain_file(handle.get());
    } catch (...) {
        return file_error<std::vector<std::byte>>(
            ProfileStoreError::io_failure,
            L"profile_store.read_file",
            L"Failed to allocate bounded profile file state");
    }
}

Result<AtomicTempFile> ProfileDirectoryCapability::create_unique_temp(
    std::wstring_view purpose) {
    try {
        if (!is_safe_leaf_name(purpose)) {
            return file_error<AtomicTempFile>(
                ProfileStoreError::io_failure,
                L"profile_store.create_temp",
                L"Temporary profile files require a safe purpose name");
        }
        auto random = product::Uuid::random();
        if (!random.ok()) {
            return Result<AtomicTempFile>::failure(random.error());
        }
        const auto narrow_id = random.value().to_string();
        const std::wstring id(narrow_id.begin(), narrow_id.end());
        const auto leaf = L"." + std::wstring(purpose) + L"." + id + L".tmp";
        const auto path = join_leaf(impl_->paths.profile_root, leaf);
        NativeHandle handle(CreateFileW(
            path.c_str(), GENERIC_READ | GENERIC_WRITE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH
                | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!handle.valid()) {
            return file_error<AtomicTempFile>(
                ProfileStoreError::io_failure,
                L"profile_store.create_temp",
                L"Failed to create a unique profile temp file", GetLastError());
        }
        CreatedFileGuard created_file(handle, path);
        auto valid = validate_plain_file(handle.get());
        if (!valid.ok()) {
            return Result<AtomicTempFile>::failure(valid.error());
        }

        std::scoped_lock lock(impl_->mutex);
        const auto token = AtomicTempFile{impl_->next_token++};
        if (token.value == 0) {
            static_cast<void>(DeleteFileW(path.c_str()));
            return file_error<AtomicTempFile>(
                ProfileStoreError::io_failure,
                L"profile_store.create_temp",
                L"Temporary profile token space was exhausted");
        }
        impl_->temps.emplace(
            token.value,
            Impl::TempState{leaf, path, std::move(handle), false, false, false});
        created_file.release();
        return Result<AtomicTempFile>::success(token);
    } catch (...) {
        return file_error<AtomicTempFile>(
            ProfileStoreError::io_failure,
            L"profile_store.create_temp",
            L"Failed to allocate temporary profile file state");
    }
}

Result<void> ProfileDirectoryCapability::write_temp(
    AtomicTempFile token, std::span<const std::byte> bytes) {
    try {
        if (bytes.empty() || bytes.size() > kProfileMaximumBytes) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.write_temp",
                L"Temporary profile bytes are empty or exceed the limit");
        }
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->temps.find(token.value);
        if (found == impl_->temps.end() || !found->second.handle.valid()) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.write_temp",
                L"Temporary profile token is invalid");
        }
        LARGE_INTEGER start{};
        if (!SetFilePointerEx(
                found->second.handle.get(), start, nullptr, FILE_BEGIN)
            || !SetEndOfFile(found->second.handle.get())) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.write_temp",
                L"Failed to truncate a temporary profile file", GetLastError());
        }
        DWORD written = 0;
        const BOOL write_ok = WriteFile(
                found->second.handle.get(), bytes.data(),
                static_cast<DWORD>(bytes.size()), &written, nullptr);
        if (!write_ok || written != static_cast<DWORD>(bytes.size())) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.write_temp",
                L"Failed to write a complete temporary profile file",
                write_ok ? ERROR_WRITE_FAULT : GetLastError());
        }
        found->second.written = true;
        found->second.flushed = false;
        found->second.verified = false;
        found->second.verified_bytes.clear();
        return Result<void>::success();
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.write_temp",
            L"Failed to access temporary profile file state");
    }
}

Result<void> ProfileDirectoryCapability::flush_temp(AtomicTempFile token) {
    try {
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->temps.find(token.value);
        if (found == impl_->temps.end() || !found->second.handle.valid()
            || !found->second.written) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.flush_temp",
                L"Temporary profile file is not ready to flush");
        }
        if (!FlushFileBuffers(found->second.handle.get())) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.flush_temp",
                L"Failed to flush temporary profile bytes", GetLastError());
        }
        found->second.flushed = true;
        found->second.verified = false;
        found->second.verified_bytes.clear();
        return Result<void>::success();
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.flush_temp",
            L"Failed to access temporary profile file state");
    }
}

Result<std::vector<std::byte>> ProfileDirectoryCapability::read_temp(
    AtomicTempFile token) {
    try {
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->temps.find(token.value);
        if (found == impl_->temps.end() || !found->second.handle.valid()
            || !found->second.flushed) {
            return file_error<std::vector<std::byte>>(
                ProfileStoreError::io_failure,
                L"profile_store.read_temp",
                L"Temporary profile file is not ready to verify");
        }
        auto result = read_plain_file(found->second.handle.get());
        if (result.ok()) {
            found->second.verified_bytes = result.value();
            found->second.verified = true;
        }
        return result;
    } catch (...) {
        return file_error<std::vector<std::byte>>(
            ProfileStoreError::io_failure,
            L"profile_store.read_temp",
            L"Failed to read temporary profile file state");
    }
}

Result<void> ProfileDirectoryCapability::publish_temp(
    AtomicTempFile token,
    std::wstring_view destination,
    bool replace_existing) {
    try {
        if (!is_safe_leaf_name(destination)) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.publish_temp",
                L"Profile destination must be a safe leaf name");
        }
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->temps.find(token.value);
        if (found == impl_->temps.end() || !found->second.handle.valid()
            || !found->second.verified) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.publish_temp",
                L"Temporary profile file is not verified for publication");
        }

        const auto target = join_leaf(impl_->paths.profile_root, destination);
        NativeHandle existing(CreateFileW(
            target.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        const bool target_exists = existing.valid();
        if (target_exists) {
            auto valid = validate_plain_file(existing.get());
            if (!valid.ok()) return valid;
        } else {
            const auto error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
                return store_error(
                    ProfileStoreError::io_failure,
                    L"profile_store.publish_temp",
                    L"Failed to inspect the profile destination", error);
            }
        }
        if (!replace_existing && target_exists) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.publish_temp",
                L"Profile destination already exists", ERROR_FILE_EXISTS);
        }
        existing.reset();
        found->second.handle.reset();

        DWORD flags = MOVEFILE_WRITE_THROUGH;
        if (replace_existing) flags |= MOVEFILE_REPLACE_EXISTING;
        if (!MoveFileExW(found->second.path.c_str(), target.c_str(), flags)) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.publish_temp",
                L"Failed to atomically publish a profile file", GetLastError());
        }
        NativeHandle published(CreateFileW(
            target.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!published.valid()) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.publish_temp",
                L"Failed to reopen the published profile file", GetLastError());
        }
        auto published_bytes = read_plain_file(published.get());
        if (!published_bytes.ok()) {
            return Result<void>::failure(published_bytes.error());
        }
        if (published_bytes.value() != found->second.verified_bytes) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.publish_temp",
                L"Published profile bytes differ from the flushed temp file");
        }
        impl_->temps.erase(found);
        return Result<void>::success();
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.publish_temp",
            L"Failed to access profile publication state");
    }
}

Result<void> ProfileDirectoryCapability::remove_file(
    std::wstring_view name) {
    try {
        if (!is_safe_leaf_name(name)) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.remove_file",
                L"Managed deletion requires a safe leaf name");
        }
        std::scoped_lock lock(impl_->mutex);
        const auto path = join_leaf(impl_->paths.profile_root, name);
        NativeHandle handle(CreateFileW(
            path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!handle.valid()) {
            const auto error = GetLastError();
            return store_error(
                error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                    ? ProfileStoreError::path_not_found
                    : ProfileStoreError::io_failure,
                L"profile_store.remove_file",
                L"Failed to open managed metadata for deletion", error);
        }
        auto valid = validate_plain_file(handle.get());
        if (!valid.ok()) return valid;
        FILE_DISPOSITION_INFO disposition{TRUE};
        if (!SetFileInformationByHandle(
                handle.get(), FileDispositionInfo,
                &disposition, sizeof(disposition))) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.remove_file",
                L"Failed to mark managed metadata for deletion",
                GetLastError());
        }
        return Result<void>::success();
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.remove_file",
            L"Failed to allocate managed deletion state");
    }
}

Result<std::vector<std::wstring>> ProfileDirectoryCapability::list_files() {
    try {
        std::scoped_lock lock(impl_->mutex);
        const auto pattern = join_leaf(impl_->paths.profile_root, L"*");
        WIN32_FIND_DATAW data{};
        NativeFindHandle find(FindFirstFileExW(
            pattern.c_str(), FindExInfoBasic, &data,
            FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH));
        if (!find.valid()) {
            const auto error = GetLastError();
            if (error == ERROR_FILE_NOT_FOUND) {
                return Result<std::vector<std::wstring>>::success({});
            }
            return file_error<std::vector<std::wstring>>(
                ProfileStoreError::io_failure,
                L"profile_store.list_files",
                L"Failed to enumerate managed metadata", error);
        }
        std::vector<std::wstring> names;
        do {
            const std::wstring_view name(data.cFileName);
            if (name != L"." && name != L"..") {
                if (!is_safe_leaf_name(name) || names.size() >= 25'000) {
                    return file_error<std::vector<std::wstring>>(
                        ProfileStoreError::io_failure,
                        L"profile_store.list_files",
                        L"Managed metadata enumeration is unsafe or excessive");
                }
                names.emplace_back(name);
            }
        } while (FindNextFileW(find.get(), &data));
        const auto error = GetLastError();
        if (error != ERROR_NO_MORE_FILES) {
            return file_error<std::vector<std::wstring>>(
                ProfileStoreError::io_failure,
                L"profile_store.list_files",
                L"Managed metadata enumeration failed", error);
        }
        return Result<std::vector<std::wstring>>::success(std::move(names));
    } catch (...) {
        return file_error<std::vector<std::wstring>>(
            ProfileStoreError::io_failure,
            L"profile_store.list_files",
            L"Failed to allocate managed enumeration state");
    }
}

Result<void> ProfileDirectoryCapability::flush_directory() {
    try {
        std::scoped_lock lock(impl_->mutex);
        if (impl_->directories.empty()
            || !FlushFileBuffers(impl_->directories.back().get())) {
            return store_error(
                ProfileStoreError::io_failure,
                L"profile_store.flush_directory",
                L"Failed to durably flush profile directory metadata",
                GetLastError());
        }
        return Result<void>::success();
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.flush_directory",
            L"Failed to access profile directory state");
    }
}

void ProfileDirectoryCapability::discard_temp(AtomicTempFile token) noexcept {
    try {
        std::scoped_lock lock(impl_->mutex);
        const auto found = impl_->temps.find(token.value);
        if (found == impl_->temps.end()) return;
        found->second.handle.reset();
        static_cast<void>(DeleteFileW(found->second.path.c_str()));
        impl_->temps.erase(found);
    } catch (...) {
    }
}

Result<ProfileDirectoryCapability> ProfileDirectoryCapability::open_existing(
    std::wstring_view trusted_program_data_root,
    const product::Uuid& profile_id) {
    try {
        auto paths = derive_profile_store_paths(
            trusted_program_data_root, profile_id);
        if (!paths.ok()) {
            return Result<ProfileDirectoryCapability>::failure(paths.error());
        }
        auto impl = std::make_unique<Impl>();
        impl->paths = paths.move_value();
        impl->directories.reserve(4);
        const std::array<std::wstring_view, 4> directories{
            trusted_program_data_root,
            impl->paths.product_root,
            impl->paths.profiles_root,
            impl->paths.profile_root};
        for (std::size_t index = 0; index < directories.size(); ++index) {
            auto opened = open_directory(
                directories[index], index + 1 == directories.size());
            if (!opened.ok()) {
                return Result<ProfileDirectoryCapability>::failure(
                    opened.error());
            }
            auto type = validate_directory_type(opened.value().get());
            if (!type.ok()) {
                return Result<ProfileDirectoryCapability>::failure(
                    type.error());
            }
            auto identity = validate_directory_identity(
                opened.value().get(), directories[index]);
            if (!identity.ok()) {
                return Result<ProfileDirectoryCapability>::failure(
                    identity.error());
            }
            if (index != 0) {
                auto acl = validate_management_acl(opened.value().get());
                if (!acl.ok()) {
                    return Result<ProfileDirectoryCapability>::failure(
                        acl.error());
                }
            }
            impl->directories.push_back(opened.move_value());
        }
        return Result<ProfileDirectoryCapability>::success(
            ProfileDirectoryCapability(std::move(impl)));
    } catch (...) {
        return Result<ProfileDirectoryCapability>::failure(Error{
            ErrorDomain::internal,
            static_cast<std::uint32_t>(ProfileStoreError::io_failure),
            L"profile_store.open_directory",
            L"Failed to allocate verified profile directory state"});
    }
}

Result<ManagedAtomicDirectory> open_managed_atomic_directory(
    std::wstring_view trusted_program_data_root,
    std::span<const std::wstring_view> managed_components) {
    try {
        if (trusted_program_data_root.empty()
            || trusted_program_data_root.find(L'\0') != std::wstring_view::npos
            || managed_components.size() < 2
            || managed_components.size() > 8
            || managed_components.front() != L"MyRamDisk") {
            return path_error<ManagedAtomicDirectory>(
                ProfileStoreError::invalid_root,
                L"Managed atomic directory arguments are invalid");
        }
        const std::filesystem::path root{
            std::wstring(trusted_program_data_root)};
        if (!root.is_absolute() || root.lexically_normal() != root) {
            return path_error<ManagedAtomicDirectory>(
                ProfileStoreError::invalid_root,
                L"The trusted ProgramData root must be absolute and normalized");
        }

        std::vector<std::wstring> directories;
        directories.reserve(managed_components.size() + 1);
        directories.push_back(root.wstring());
        auto current = root;
        for (const auto component : managed_components) {
            if (!is_safe_leaf_name(component)) {
                return path_error<ManagedAtomicDirectory>(
                    ProfileStoreError::invalid_root,
                    L"A managed directory component is unsafe");
            }
            current /= std::wstring(component);
            directories.push_back(current.wstring());
        }

        auto impl = std::make_unique<ProfileDirectoryCapability::Impl>();
        impl->paths.product_root = directories[1];
        impl->paths.profile_root = directories.back();
        impl->directories.reserve(directories.size());
        for (std::size_t index = 0; index < directories.size(); ++index) {
            auto opened = open_directory(
                directories[index], index + 1 == directories.size());
            if (!opened.ok()) {
                return Result<ManagedAtomicDirectory>::failure(opened.error());
            }
            auto type = validate_directory_type(opened.value().get());
            if (!type.ok()) {
                return Result<ManagedAtomicDirectory>::failure(type.error());
            }
            auto identity = validate_directory_identity(
                opened.value().get(), directories[index]);
            if (!identity.ok()) {
                return Result<ManagedAtomicDirectory>::failure(identity.error());
            }
            if (index != 0) {
                auto acl = validate_management_acl(opened.value().get());
                if (!acl.ok()) {
                    return Result<ManagedAtomicDirectory>::failure(acl.error());
                }
            }
            impl->directories.push_back(opened.move_value());
        }

        std::unique_ptr<IAtomicFileDirectory> directory(
            new ProfileDirectoryCapability(std::move(impl)));
        return Result<ManagedAtomicDirectory>::success(
            ManagedAtomicDirectory{directories.back(), std::move(directory)});
    } catch (...) {
        return path_error<ManagedAtomicDirectory>(
            ProfileStoreError::io_failure,
            L"Failed to allocate managed atomic directory state");
    }
}

namespace {

template <class T>
Result<T> profile_state_error(ProfileStoreError code,
                              const wchar_t* operation,
                              const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(code), operation, message});
}

Result<ProfileRecord> load_profile_record(
    IAtomicFileDirectory& directory,
    std::wstring_view leaf,
    const product::Uuid& expected_id,
    ProfileStoreError corruption_code) {
    auto bytes = directory.read_file(leaf);
    if (!bytes.ok()) {
        if (bytes.error().code == static_cast<std::uint32_t>(
                ProfileStoreError::invalid_file_size)) {
            return profile_state_error<ProfileRecord>(
                corruption_code,
                L"profile_store.load",
                L"The persisted profile has an invalid file size");
        }
        return Result<ProfileRecord>::failure(bytes.error());
    }
    auto decoded = decode_profile(bytes.value());
    if (!decoded.ok()) {
        return profile_state_error<ProfileRecord>(
            corruption_code,
            leaf == L"profile.myrd"
                ? L"profile_store.load_current"
                : L"profile_store.load_previous",
            leaf == L"profile.myrd"
                ? L"The current profile is corrupt"
                : L"The previous profile is corrupt");
    }
    if (decoded.value().profile.profile_uuid != expected_id) {
        return profile_state_error<ProfileRecord>(
            ProfileStoreError::profile_id_mismatch,
            L"profile_store.load",
            L"The profile UUID does not match its directory binding");
    }
    const auto validation = validate_profile(decoded.value());
    if (!validation.ok()) {
        return profile_state_error<ProfileRecord>(
            corruption_code,
            L"profile_store.load",
            L"The persisted profile fails semantic validation");
    }
    return decoded;
}

std::uint16_t profile_u16(
    std::span<const std::byte> bytes, std::size_t offset) noexcept {
    return static_cast<std::uint16_t>(
        std::to_integer<std::uint8_t>(bytes[offset])
        | (static_cast<std::uint16_t>(
               std::to_integer<std::uint8_t>(bytes[offset + 1]))
           << 8U));
}

std::uint32_t profile_u32(
    std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index) {
        value |= static_cast<std::uint32_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

std::uint64_t profile_u64(
    std::span<const std::byte> bytes, std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index) {
        value |= static_cast<std::uint64_t>(
                     std::to_integer<std::uint8_t>(bytes[offset + index]))
                 << (index * 8U);
    }
    return value;
}

Result<std::uint64_t> claimed_profile_revision(
    std::span<const std::byte> bytes) {
    if (bytes.size() < kProfileHeaderBytes
        || bytes.size() > kProfileMaximumBytes
        || !std::equal(
            kProfileMagic.begin(), kProfileMagic.end(), bytes.begin())
        || profile_u16(bytes, 8) != kProfileSchemaMajor
        || profile_u16(bytes, 10) != kProfileSchemaMinor
        || profile_u32(bytes, 12) != kProfileHeaderBytes
        || profile_u64(bytes, 16) != bytes.size()
        || profile_u32(bytes, 32) != bytes.size() - kProfileHeaderBytes
        || profile_u32(bytes, 36) < kProfileKnownFieldCount
        || profile_u32(bytes, 36) > kProfileMaximumFields
        || profile_u64(bytes, 24) == 0) {
        return profile_state_error<std::uint64_t>(
            ProfileStoreError::repair_precondition_failed,
            L"profile_store.repair",
            L"The corrupt current profile has no trustworthy revision claim");
    }
    return Result<std::uint64_t>::success(profile_u64(bytes, 24));
}

Result<void> validation_failure(
    const ProfileValidationResult& validation) {
    return Result<void>::failure(Error{
        ErrorDomain::protocol,
        static_cast<std::uint32_t>(ProfileStoreError::validation_failed),
        L"profile_store.write_next",
        std::wstring(validation.public_message)});
}

std::shared_ptr<std::mutex> shared_profile_writer_gate(
    const product::Uuid& profile_id) {
    static std::mutex registry_mutex;
    static std::map<std::string, std::weak_ptr<std::mutex>, std::less<>> registry;
    const auto key = profile_id.to_string();
    std::scoped_lock lock(registry_mutex);
    if (const auto found = registry.find(key); found != registry.end()) {
        if (auto existing = found->second.lock()) return existing;
        registry.erase(found);
    }
    auto created = std::make_shared<std::mutex>();
    registry.emplace(key, created);
    for (auto iterator = registry.begin(); iterator != registry.end();) {
        if (iterator->second.expired()) {
            iterator = registry.erase(iterator);
        } else {
            ++iterator;
        }
    }
    return created;
}

} // namespace

struct ProfileStore::Impl final {
    Impl(std::unique_ptr<IAtomicFileDirectory> directory_value,
         ProfileStorePaths paths_value,
         product::Uuid profile_id_value,
         std::shared_ptr<std::mutex> writer_gate_value)
        : directory(std::move(directory_value)),
          paths(std::move(paths_value)),
          profile_id(std::move(profile_id_value)),
          writer_gate(std::move(writer_gate_value)) {}

    std::unique_ptr<IAtomicFileDirectory> directory;
    ProfileStorePaths paths;
    product::Uuid profile_id;
    std::shared_ptr<std::mutex> writer_gate;
};

ProfileStore::ProfileStore(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

ProfileStore::ProfileStore(ProfileStore&&) noexcept = default;
ProfileStore& ProfileStore::operator=(ProfileStore&&) noexcept = default;
ProfileStore::~ProfileStore() noexcept = default;

Result<ProfileStore> ProfileStore::open_existing(
    std::wstring_view trusted_program_data_root,
    const product::Uuid& profile_id) {
    try {
        auto capability = ProfileDirectoryCapability::open_existing(
            trusted_program_data_root, profile_id);
        if (!capability.ok()) {
            return Result<ProfileStore>::failure(capability.error());
        }
        auto paths = capability.value().paths();
        auto directory = std::make_unique<ProfileDirectoryCapability>(
            capability.move_value());
        return from_directory_capability(
            std::move(directory), std::move(paths), profile_id);
    } catch (...) {
        return profile_state_error<ProfileStore>(
            ProfileStoreError::io_failure,
            L"profile_store.open",
            L"Failed to allocate profile store state");
    }
}

Result<ProfileStore> ProfileStore::from_directory_capability(
    std::unique_ptr<IAtomicFileDirectory> directory,
    ProfileStorePaths paths,
    const product::Uuid& profile_id) {
    try {
        if (!directory || is_zero(profile_id)) {
            return profile_state_error<ProfileStore>(
                ProfileStoreError::invalid_profile_id,
                L"profile_store.open",
                L"Profile store capability and UUID are required");
        }
        auto writer_gate = shared_profile_writer_gate(profile_id);
        auto impl = std::make_unique<Impl>(
            std::move(directory), std::move(paths), profile_id,
            std::move(writer_gate));
        return Result<ProfileStore>::success(ProfileStore(std::move(impl)));
    } catch (...) {
        return profile_state_error<ProfileStore>(
            ProfileStoreError::io_failure,
            L"profile_store.open",
            L"Failed to allocate profile store state");
    }
}

const ProfileStorePaths& ProfileStore::paths() const noexcept {
    return impl_->paths;
}

Result<ProfileRecord> ProfileStore::load_current() {
    try {
        return load_profile_record(
            *impl_->directory, L"profile.myrd", impl_->profile_id,
            ProfileStoreError::current_corrupt);
    } catch (...) {
        return profile_state_error<ProfileRecord>(
            ProfileStoreError::io_failure,
            L"profile_store.load_current",
            L"Failed to allocate current profile state");
    }
}

Result<ProfileRecord> ProfileStore::load_previous() {
    try {
        return load_profile_record(
            *impl_->directory, L"profile.prev", impl_->profile_id,
            ProfileStoreError::previous_corrupt);
    } catch (...) {
        return profile_state_error<ProfileRecord>(
            ProfileStoreError::io_failure,
            L"profile_store.load_previous",
            L"Failed to allocate previous profile state");
    }
}

Result<void> ProfileStore::write_next(
    std::uint64_t expected_revision,
    const ProfileRecord& proposed,
    bool profile_is_running) {
    try {
        std::scoped_lock lock(*impl_->writer_gate);
        auto current_bytes = impl_->directory->read_file(L"profile.myrd");
        const bool current_exists = current_bytes.ok();
        if (!current_exists
            && current_bytes.error().code != static_cast<std::uint32_t>(
                ProfileStoreError::path_not_found)) {
            return Result<void>::failure(current_bytes.error());
        }

        std::optional<ProfileRecord> current;
        if (current_exists) {
            auto decoded = decode_profile(current_bytes.value());
            if (!decoded.ok() || !validate_profile(decoded.value()).ok()) {
                return store_error(
                    ProfileStoreError::current_corrupt,
                    L"profile_store.write_next",
                    L"The current profile is corrupt");
            }
            if (decoded.value().profile.profile_uuid != impl_->profile_id) {
                return store_error(
                    ProfileStoreError::profile_id_mismatch,
                    L"profile_store.write_next",
                    L"The current profile UUID does not match its directory");
            }
            current = decoded.move_value();
        }

        if ((!current.has_value() && expected_revision != 0)
            || (current.has_value()
                && current->revision != expected_revision)) {
            return store_error(
                ProfileStoreError::revision_conflict,
                L"profile_store.write_next",
                L"The expected profile revision is stale");
        }
        if (proposed.profile.profile_uuid != impl_->profile_id) {
            return store_error(
                ProfileStoreError::profile_id_mismatch,
                L"profile_store.write_next",
                L"The proposed profile UUID does not match its directory");
        }

        const auto validation = current.has_value()
            ? validate_profile_update(
                current.value(), proposed, profile_is_running)
            : validate_profile(proposed);
        if (!validation.ok()) return validation_failure(validation);

        auto encoded = encode_profile(proposed);
        if (!encoded.ok()) return Result<void>::failure(encoded.error());
        auto round_trip = decode_profile(encoded.value());
        if (!round_trip.ok() || round_trip.value() != proposed) {
            return store_error(
                ProfileStoreError::validation_failed,
                L"profile_store.write_next",
                L"Encoded profile failed its publication round trip");
        }
        return publish_atomically_with_previous(
            *impl_->directory, L"profile.myrd", L"profile.prev",
            encoded.value());
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.write_next",
            L"Profile write failed with an internal exception");
    }
}

Result<void> ProfileStore::write_rebind(
    std::uint64_t expected_revision,
    const ProfileRecord& proposed) {
    try {
        std::scoped_lock lock(*impl_->writer_gate);
        auto current = load_profile_record(
            *impl_->directory, L"profile.myrd", impl_->profile_id,
            ProfileStoreError::current_corrupt);
        if (!current.ok()) return Result<void>::failure(current.error());
        if (current.value().revision != expected_revision) {
            return store_error(
                ProfileStoreError::revision_conflict,
                L"profile_store.write_rebind",
                L"The expected profile revision is stale");
        }
        if (proposed.profile.profile_uuid != impl_->profile_id) {
            return store_error(
                ProfileStoreError::profile_id_mismatch,
                L"profile_store.write_rebind",
                L"The proposed profile UUID does not match its directory");
        }
        const auto validation = validate_profile_rebind(
            current.value(), proposed);
        if (!validation.ok()) return validation_failure(validation);
        auto encoded = encode_profile(proposed);
        if (!encoded.ok()) return Result<void>::failure(encoded.error());
        auto round_trip = decode_profile(encoded.value());
        if (!round_trip.ok() || round_trip.value() != proposed) {
            return store_error(
                ProfileStoreError::validation_failed,
                L"profile_store.write_rebind",
                L"Encoded rebind profile failed its publication round trip");
        }
        return publish_atomically_with_previous(
            *impl_->directory, L"profile.myrd", L"profile.prev",
            encoded.value());
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.write_rebind",
            L"Profile rebind failed with an internal exception");
    }
}

Result<std::uint64_t> ProfileStore::claimed_current_revision() {
    try {
        std::scoped_lock lock(*impl_->writer_gate);
        auto current = impl_->directory->read_file(L"profile.myrd");
        if (!current.ok()) {
            return Result<std::uint64_t>::failure(current.error());
        }
        return claimed_profile_revision(current.value());
    } catch (...) {
        return profile_state_error<std::uint64_t>(
            ProfileStoreError::io_failure,
            L"profile_store.claimed_current_revision",
            L"Failed to inspect the current profile revision claim");
    }
}

Result<void> ProfileStore::repair_from_previous(
    std::uint64_t expected_claimed_current_revision,
    IProfileRepairBindingVerifier& verifier) {
    try {
        std::scoped_lock lock(*impl_->writer_gate);
        auto current_bytes = impl_->directory->read_file(L"profile.myrd");
        if (!current_bytes.ok()) {
            return Result<void>::failure(current_bytes.error());
        }
        auto decoded_current = decode_profile(current_bytes.value());
        if (decoded_current.ok()
            && decoded_current.value().profile.profile_uuid
                != impl_->profile_id) {
            return store_error(
                ProfileStoreError::profile_id_mismatch,
                L"profile_store.repair",
                L"A valid current profile belongs to another directory");
        }
        if (decoded_current.ok()
            && validate_profile(decoded_current.value()).ok()) {
            return store_error(
                ProfileStoreError::repair_precondition_failed,
                L"profile_store.repair",
                L"The current profile is valid and does not require repair");
        }
        auto claimed = claimed_profile_revision(current_bytes.value());
        if (!claimed.ok()) return Result<void>::failure(claimed.error());
        if (claimed.value() != expected_claimed_current_revision) {
            return store_error(
                ProfileStoreError::revision_conflict,
                L"profile_store.repair",
                L"The expected corrupt-current revision claim is stale");
        }

        auto previous = load_profile_record(
            *impl_->directory, L"profile.prev", impl_->profile_id,
            ProfileStoreError::previous_corrupt);
        if (!previous.ok()) return Result<void>::failure(previous.error());
        auto verified = verifier.verify_repair_binding(previous.value());
        if (!verified.ok()) return verified;

        const auto base_revision = (std::max)(
            claimed.value(), previous.value().revision);
        if (base_revision == (std::numeric_limits<std::uint64_t>::max)()) {
            return store_error(
                ProfileStoreError::revision_exhausted,
                L"profile_store.repair",
                L"No higher profile revision can be represented");
        }
        auto repaired = previous.value();
        repaired.revision = base_revision + 1;
        const auto validation = validate_profile(repaired);
        if (!validation.ok()) return validation_failure(validation);
        auto encoded = encode_profile(repaired);
        if (!encoded.ok()) return Result<void>::failure(encoded.error());
        auto round_trip = decode_profile(encoded.value());
        if (!round_trip.ok() || round_trip.value() != repaired) {
            return store_error(
                ProfileStoreError::validation_failed,
                L"profile_store.repair",
                L"Encoded repair profile failed its publication round trip");
        }
        return publish_current_preserving_previous(
            *impl_->directory, L"profile.myrd", encoded.value());
    } catch (...) {
        return store_error(
            ProfileStoreError::io_failure,
            L"profile_store.repair",
            L"Profile repair failed with an internal exception");
    }
}

} // namespace myrd::profile
