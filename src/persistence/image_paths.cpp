#include "image_paths.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "src/persistence/persistence_types.h"
#include "src/persistence/writer_lock.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <limits>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace myrd::persistence {
namespace {

[[nodiscard]] Error path_error(
    const ImagePathErrorCode code,
    std::wstring operation,
    std::wstring message,
    const std::uint32_t native_code = 0) {
    return {
        ErrorDomain::storage,
        static_cast<std::uint32_t>(code),
        std::move(operation),
        std::move(message),
        native_code,
    };
}

[[nodiscard]] Result<ImageComponent> invalid_name(
    const wchar_t* message) {
    return Result<ImageComponent>::failure(path_error(
        ImagePathErrorCode::InvalidName,
        L"image_path.validate_name",
        message));
}

template <class T>
[[nodiscard]] Result<T> path_failure(
    const ImagePathErrorCode code,
    const wchar_t* operation,
    const wchar_t* message,
    const std::uint32_t native_code = 0) {
    return Result<T>::failure(path_error(
        code, operation, message, native_code));
}

[[nodiscard]] ImagePathErrorCode native_open_error_code(
    const std::uint32_t code) noexcept {
    switch (code) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ImagePathErrorCode::Missing;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return ImagePathErrorCode::AccessDenied;
    default:
        return ImagePathErrorCode::IoError;
    }
}

[[nodiscard]] bool equals_ordinal_ignore_case(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        right.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return false;
    }
    return CompareStringOrdinal(
        left.data(), static_cast<int>(left.size()),
        right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool is_dos_device_name(
    const std::wstring_view name) noexcept {
    const auto dot = name.find(L'.');
    const auto base = name.substr(0, dot);
    constexpr std::array<std::wstring_view, 8> fixed = {
        L"CON", L"PRN", L"AUX", L"NUL",
        L"CONIN$", L"CONOUT$", L"CLOCK$", L"CONFIG$",
    };
    if (std::any_of(fixed.begin(), fixed.end(), [base](const auto candidate) {
            return equals_ordinal_ignore_case(base, candidate);
        })) {
        return true;
    }
    if (base.size() != 4) {
        return false;
    }
    const auto prefix = base.substr(0, 3);
    if (!equals_ordinal_ignore_case(prefix, L"COM") &&
        !equals_ordinal_ignore_case(prefix, L"LPT")) {
        return false;
    }
    const auto suffix = base[3];
    return (suffix >= L'1' && suffix <= L'9') ||
        suffix == L'\u00B9' || suffix == L'\u00B2' || suffix == L'\u00B3';
}

[[nodiscard]] bool exact_identity_match(
    const WindowsFileIdentity& left,
    const WindowsFileIdentity& right) noexcept {
    return left == right;
}

struct OpenedBundleFile final {
    UniqueHandle root_handle;
    UniqueHandle file_handle;
    WindowsFileIdentity root_identity{};
    WindowsFileIdentity file_identity{};
    std::wstring root_final_path;
    std::wstring file_final_path;
    std::string relative_name;
};

[[nodiscard]] Result<OpenedBundleFile> open_bundle_file_handles(
    const std::filesystem::path& bundle_root,
    const std::string_view relative_name,
    const std::optional<WindowsFileIdentity>& expected_identity,
    const bool mutable_access) {
    auto component = validate_image_component(relative_name);
    if (!component.ok()) {
        return Result<OpenedBundleFile>::failure(
            std::move(component.error()));
    }

    UniqueHandle root_handle(CreateFileW(
        bundle_root.c_str(),
        FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (!root_handle) {
        const auto native_code = GetLastError();
        return path_failure<OpenedBundleFile>(
            native_open_error_code(native_code),
            L"image_path.open_root",
            L"opening the bundle directory failed",
            native_code);
    }

    auto root_identity = query_windows_file_identity(root_handle.get());
    if (!root_identity.ok()) {
        return path_failure<OpenedBundleFile>(
            ImagePathErrorCode::IoError,
            L"image_path.open_root",
            L"querying the bundle directory identity failed",
            root_identity.error().native_code);
    }
    if ((root_identity.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        root_identity.value().reparse_tag != 0) {
        return path_failure<OpenedBundleFile>(
            ImagePathErrorCode::ReparsePoint,
            L"image_path.open_root",
            L"the bundle directory itself is a reparse point");
    }
    if ((root_identity.value().attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        return path_failure<OpenedBundleFile>(
            ImagePathErrorCode::PathEscape,
            L"image_path.open_root",
            L"the bundle root is not a directory");
    }

    auto root_final_path = normalized_final_path(root_handle.get());
    if (!root_final_path.ok()) {
        return Result<OpenedBundleFile>::failure(
            std::move(root_final_path.error()));
    }

    try {
        std::wstring target_path = root_final_path.value();
        if (!target_path.empty() &&
            target_path.back() != L'\\' && target_path.back() != L'/') {
            target_path.push_back(L'\\');
        }
        target_path.append(component.value().wide_name);

        const DWORD access = mutable_access
            ? (GENERIC_READ | GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES)
            : (GENERIC_READ | FILE_READ_ATTRIBUTES);
        const DWORD sharing = mutable_access
            ? FILE_SHARE_READ
            : (FILE_SHARE_READ | FILE_SHARE_WRITE);
        UniqueHandle file_handle(CreateFileW(
            target_path.c_str(),
            access,
            sharing,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!file_handle) {
            const auto native_code = GetLastError();
            return path_failure<OpenedBundleFile>(
                native_open_error_code(native_code),
                L"image_path.open_target",
                L"opening the bundle file failed",
                native_code);
        }

        auto file_identity = query_windows_file_identity(file_handle.get());
        if (!file_identity.ok()) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::IoError,
                L"image_path.open_target",
                L"querying the bundle file identity failed",
                file_identity.error().native_code);
        }
        if ((file_identity.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
            file_identity.value().reparse_tag != 0) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::ReparsePoint,
                L"image_path.open_target",
                L"bundle files cannot be reparse points");
        }
        if ((file_identity.value().attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::PathEscape,
                L"image_path.open_target",
                L"bundle file target is a directory");
        }
        if (file_identity.value().hard_link_count != 1) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::HardLink,
                L"image_path.open_target",
                L"bundle files must have exactly one hard link");
        }

        auto contained = is_direct_bundle_child(
            root_handle.get(), file_handle.get());
        if (!contained.ok()) {
            return Result<OpenedBundleFile>::failure(
                std::move(contained.error()));
        }
        if (!contained.value()) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::PathEscape,
                L"image_path.open_target",
                L"opened target is not a direct child of the bundle root");
        }

        if (expected_identity.has_value() &&
            !exact_identity_match(*expected_identity, file_identity.value())) {
            return path_failure<OpenedBundleFile>(
                ImagePathErrorCode::IdentityMismatch,
                L"image_path.open_target",
                L"opened bundle file identity differs from the expected identity");
        }

        auto file_final_path = normalized_final_path(file_handle.get());
        if (!file_final_path.ok()) {
            return Result<OpenedBundleFile>::failure(
                std::move(file_final_path.error()));
        }
        return Result<OpenedBundleFile>::success({
            std::move(root_handle),
            std::move(file_handle),
            root_identity.move_value(),
            file_identity.move_value(),
            root_final_path.move_value(),
            file_final_path.move_value(),
            component.value().utf8_name,
        });
    } catch (const std::bad_alloc&) {
        return path_failure<OpenedBundleFile>(
            ImagePathErrorCode::IoError,
            L"image_path.open_target",
            L"allocating bundle path state failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

[[nodiscard]] Result<void> verify_cleanup_capability_locked(
    const detail::WriterAuthorityState& writer_state,
    const std::shared_ptr<detail::WriterAuthorityState>& authority,
    const bool deleted,
    HANDLE capability_root,
    HANDLE capability_file,
    const WindowsFileIdentity& capability_root_identity,
    const WindowsFileIdentity& capability_identity) {
    if (deleted) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.cleanup",
            L"cleanup requires a live writer and file capability");
    }
    if (authority == nullptr || authority.get() != &writer_state) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.cleanup",
            L"file capability belongs to a different writer lifetime");
    }

    auto writer_valid = detail::verify_writer_state_locked(writer_state);
    if (!writer_valid.ok()) {
        return writer_valid;
    }

    auto root_identity = query_windows_file_identity(capability_root);
    auto current_identity = query_windows_file_identity(capability_file);
    if (!root_identity.ok() || !current_identity.ok()) {
        const auto native_code = !root_identity.ok()
            ? root_identity.error().native_code
            : current_identity.error().native_code;
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.cleanup",
            L"cleanup identity recheck failed",
            native_code);
    }
    if (!same_file_object(root_identity.value(), capability_root_identity) ||
        !same_file_object(root_identity.value(), writer_state.root_identity)) {
        return path_failure<void>(
            ImagePathErrorCode::PathEscape,
            L"image_path.cleanup",
            L"bundle root identity changed before cleanup");
    }
    if ((current_identity.value().attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        current_identity.value().reparse_tag != 0) {
        return path_failure<void>(
            ImagePathErrorCode::ReparsePoint,
            L"image_path.cleanup",
            L"bundle file became a reparse point before cleanup");
    }
    if (current_identity.value().hard_link_count != 1) {
        return path_failure<void>(
            ImagePathErrorCode::HardLink,
            L"image_path.cleanup",
            L"bundle file gained another hard link before cleanup");
    }
    if (!exact_identity_match(current_identity.value(), capability_identity)) {
        return path_failure<void>(
            ImagePathErrorCode::IdentityMismatch,
            L"image_path.cleanup",
            L"bundle file identity changed before cleanup");
    }
    auto contained = is_direct_bundle_child(
        capability_root, capability_file);
    if (!contained.ok()) {
        return Result<void>::failure(std::move(contained.error()));
    }
    if (!contained.value()) {
        return path_failure<void>(
            ImagePathErrorCode::PathEscape,
            L"image_path.cleanup",
            L"bundle file left the bundle root before cleanup");
    }
    return Result<void>::success();
}

} // namespace

Result<ImageComponent> validate_image_component(
    const std::string_view utf8_name) {
    if (utf8_name.empty() || utf8_name.size() > kMaxRelativeNameBytes) {
        return invalid_name(L"image filename must contain 1..255 UTF-8 bytes");
    }
    if (utf8_name.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return invalid_name(L"image filename is too long for UTF-8 validation");
    }

    const auto byte_count = static_cast<int>(utf8_name.size());
    const auto wide_count = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        utf8_name.data(),
        byte_count,
        nullptr,
        0);
    if (wide_count <= 0) {
        return invalid_name(L"image filename is not valid UTF-8");
    }

    try {
        std::wstring wide_name(static_cast<std::size_t>(wide_count), L'\0');
        if (MultiByteToWideChar(
                CP_UTF8,
                MB_ERR_INVALID_CHARS,
                utf8_name.data(),
                byte_count,
                wide_name.data(),
                wide_count) != wide_count) {
            return invalid_name(L"image filename UTF-8 conversion failed");
        }

        if (wide_name == L"." || wide_name == L".." ||
            wide_name.back() == L'.' || wide_name.back() == L' ') {
            return invalid_name(L"image filename uses a reserved component form");
        }
        constexpr std::wstring_view forbidden = L"<>:\"/\\|?*";
        for (const auto character : wide_name) {
            if (character < L' ' || forbidden.find(character) != std::wstring_view::npos) {
                return invalid_name(L"image filename contains a forbidden character");
            }
        }
        if (is_dos_device_name(wide_name)) {
            return invalid_name(L"image filename is a reserved DOS device name");
        }

        return Result<ImageComponent>::success({
            std::string(utf8_name),
            std::move(wide_name),
        });
    } catch (const std::bad_alloc&) {
        return Result<ImageComponent>::failure(path_error(
            ImagePathErrorCode::IoError,
            L"image_path.validate_name",
            L"allocating the validated image filename failed",
            ERROR_NOT_ENOUGH_MEMORY));
    }
}

ValidatedBundleFile::ValidatedBundleFile(
    UniqueHandle root_handle,
    UniqueHandle file_handle,
    WindowsFileIdentity root_identity,
    WindowsFileIdentity identity,
    std::wstring root_final_path,
    std::wstring final_path,
    std::string relative_name) noexcept
    : root_handle_(std::move(root_handle)),
      file_handle_(std::move(file_handle)),
      root_identity_(std::move(root_identity)),
      identity_(std::move(identity)),
      root_final_path_(std::move(root_final_path)),
      final_path_(std::move(final_path)),
      relative_name_(std::move(relative_name)) {}

bool ValidatedBundleFile::writer_authorized() const noexcept {
    auto authority = writer_authority_.lock();
    if (authority == nullptr) {
        return false;
    }
    std::scoped_lock lock(authority->mutex);
    return authority->active;
}

Result<std::wstring> normalized_final_path(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return path_failure<std::wstring>(
            ImagePathErrorCode::IoError,
            L"image_path.final_path",
            L"normalized final path requires a valid handle",
            ERROR_INVALID_HANDLE);
    }
    constexpr DWORD kFlags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    const auto required = GetFinalPathNameByHandleW(handle, nullptr, 0, kFlags);
    if (required == 0) {
        const auto native_code = GetLastError();
        return path_failure<std::wstring>(
            ImagePathErrorCode::IoError,
            L"image_path.final_path",
            L"querying normalized final path length failed",
            native_code);
    }
    constexpr DWORD kMaximumFinalPathChars = 32768;
    if (required > kMaximumFinalPathChars) {
        return path_failure<std::wstring>(
            ImagePathErrorCode::PathEscape,
            L"image_path.final_path",
            L"normalized final path exceeds the hard limit");
    }
    try {
        std::wstring path(static_cast<std::size_t>(required), L'\0');
        const auto written = GetFinalPathNameByHandleW(
            handle, path.data(), required, kFlags);
        if (written == 0 || written >= required) {
            const auto native_code = GetLastError();
            return path_failure<std::wstring>(
                ImagePathErrorCode::IoError,
                L"image_path.final_path",
                L"querying normalized final path failed",
                native_code);
        }
        path.resize(written);
        return Result<std::wstring>::success(std::move(path));
    } catch (const std::bad_alloc&) {
        return path_failure<std::wstring>(
            ImagePathErrorCode::IoError,
            L"image_path.final_path",
            L"allocating normalized final path failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<bool> is_direct_bundle_child(HANDLE bundle_root, HANDLE target) {
    auto root_path = normalized_final_path(bundle_root);
    if (!root_path.ok()) {
        return Result<bool>::failure(std::move(root_path.error()));
    }
    auto target_path = normalized_final_path(target);
    if (!target_path.ok()) {
        return Result<bool>::failure(std::move(target_path.error()));
    }
    try {
        const auto parent = std::filesystem::path(target_path.value())
            .parent_path();
        UniqueHandle parent_handle(CreateFileW(
            parent.c_str(),
            FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
            nullptr));
        if (!parent_handle) {
            const auto native_code = GetLastError();
            return path_failure<bool>(
                native_open_error_code(native_code),
                L"image_path.containment",
                L"opening the target parent for identity comparison failed",
                native_code);
        }
        auto root_identity = query_windows_file_identity(bundle_root);
        auto parent_identity = query_windows_file_identity(parent_handle.get());
        if (!root_identity.ok() || !parent_identity.ok()) {
            const auto native_code = !root_identity.ok()
                ? root_identity.error().native_code
                : parent_identity.error().native_code;
            return path_failure<bool>(
                ImagePathErrorCode::IoError,
                L"image_path.containment",
                L"querying parent identity for containment failed",
                native_code);
        }
        return Result<bool>::success(same_file_object(
            root_identity.value(), parent_identity.value()));
    } catch (const std::bad_alloc&) {
        return path_failure<bool>(
            ImagePathErrorCode::IoError,
            L"image_path.containment",
            L"allocating containment path state failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<ValidatedBundleFile> open_bundle_file_for_inspection(
    const std::filesystem::path& bundle_root,
    const std::string_view relative_name,
    const std::optional<WindowsFileIdentity> expected_identity) {
    auto opened = open_bundle_file_handles(
        bundle_root, relative_name, expected_identity, false);
    if (!opened.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(opened.error()));
    }
    auto value = opened.move_value();
    return Result<ValidatedBundleFile>::success(ValidatedBundleFile(
        std::move(value.root_handle),
        std::move(value.file_handle),
        std::move(value.root_identity),
        std::move(value.file_identity),
        std::move(value.root_final_path),
        std::move(value.file_final_path),
        std::move(value.relative_name)));
}

Result<ValidatedBundleFile> open_bundle_file_for_mutation(
    BundleWriterLock& writer_lock,
    const std::string_view relative_name,
    const std::optional<WindowsFileIdentity> expected_identity) {
    if (writer_lock.state_ == nullptr) {
        return path_failure<ValidatedBundleFile>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.open_mutable",
            L"mutable bundle file requires a live writer lock");
    }
    std::scoped_lock lock(writer_lock.state_->mutex);
    auto writer_valid = detail::verify_writer_state_locked(*writer_lock.state_);
    if (!writer_valid.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(writer_valid.error()));
    }
    auto opened = open_bundle_file_handles(
        writer_lock.state_->root_path,
        relative_name,
        expected_identity,
        true);
    if (!opened.ok()) {
        return Result<ValidatedBundleFile>::failure(
            std::move(opened.error()));
    }
    auto value = opened.move_value();
    ValidatedBundleFile capability(
        std::move(value.root_handle),
        std::move(value.file_handle),
        std::move(value.root_identity),
        std::move(value.file_identity),
        std::move(value.root_final_path),
        std::move(value.file_final_path),
        std::move(value.relative_name));
    capability.writer_authority_ = writer_lock.state_;
    return Result<ValidatedBundleFile>::success(std::move(capability));
}

Result<void> truncate_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::uint64_t new_length) {
    if (new_length > static_cast<std::uint64_t>(
            (std::numeric_limits<std::int64_t>::max)())) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.truncate",
            L"truncate length exceeds the signed Windows file limit",
            ERROR_ARITHMETIC_OVERFLOW);
    }
    if (writer_lock.state_ == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.truncate",
            L"truncate requires a live writer lock");
    }
    std::scoped_lock lock(writer_lock.state_->mutex);
    auto allowed = verify_cleanup_capability_locked(
        *writer_lock.state_,
        file.writer_authority_.lock(),
        file.deleted_,
        file.root_handle_.get(),
        file.file_handle_.get(),
        file.root_identity_,
        file.identity_);
    if (!allowed.ok()) {
        return allowed;
    }
    FILE_END_OF_FILE_INFO info{};
    info.EndOfFile.QuadPart = static_cast<LONGLONG>(new_length);
    if (!SetFileInformationByHandle(
            file.file_handle_.get(), FileEndOfFileInfo, &info, sizeof(info))) {
        const auto native_code = GetLastError();
        return path_failure<void>(
            native_open_error_code(native_code),
            L"image_path.truncate",
            L"truncating the validated bundle file failed",
            native_code);
    }
    auto refreshed = query_windows_file_identity(file.file_handle_.get());
    if (!refreshed.ok()) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.truncate",
            L"refreshing identity after truncate failed",
            refreshed.error().native_code);
    }
    file.identity_ = refreshed.move_value();
    return Result<void>::success();
}

Result<void> rename_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file,
    const std::string_view new_relative_name) {
    auto component = validate_image_component(new_relative_name);
    if (!component.ok()) {
        return Result<void>::failure(std::move(component.error()));
    }
    if (writer_lock.state_ == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.rename",
            L"rename requires a live writer lock");
    }
    std::scoped_lock lock(writer_lock.state_->mutex);
    auto allowed = verify_cleanup_capability_locked(
        *writer_lock.state_,
        file.writer_authority_.lock(),
        file.deleted_,
        file.root_handle_.get(),
        file.file_handle_.get(),
        file.root_identity_,
        file.identity_);
    if (!allowed.ok()) {
        return allowed;
    }

    try {
        // FileRenameInfo is most portable with an absolute normalized target;
        // RootDirectory-relative names are rejected with ERROR_INVALID_PARAMETER
        // by some SetFileInformationByHandle implementations.
        std::wstring destination = file.root_final_path_;
        if (!destination.empty() &&
            destination.back() != L'\\' && destination.back() != L'/') {
            destination.push_back(L'\\');
        }
        destination.append(component.value().wide_name);
        const auto name_bytes = destination.size() * sizeof(wchar_t);
        // SetFileInformationByHandle validates at least
        // sizeof(FILE_RENAME_INFO), including its declared first WCHAR.
        const auto total_bytes = sizeof(FILE_RENAME_INFO) + name_bytes;
        if (name_bytes > (std::numeric_limits<DWORD>::max)() ||
            total_bytes > (std::numeric_limits<DWORD>::max)()) {
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.rename",
                L"rename information exceeds the Windows buffer limit",
                ERROR_ARITHMETIC_OVERFLOW);
        }
        std::vector<std::byte> storage(total_bytes);
        auto* const info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = FALSE;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(name_bytes);
        std::memcpy(
            info->FileName,
            destination.data(),
            name_bytes);
        if (!SetFileInformationByHandle(
                file.file_handle_.get(),
                FileRenameInfo,
                info,
                static_cast<DWORD>(total_bytes))) {
            const auto native_code = GetLastError();
            return path_failure<void>(
                native_open_error_code(native_code),
                L"image_path.rename",
                L"renaming the validated bundle file failed",
                native_code);
        }
        auto contained = is_direct_bundle_child(
            file.root_handle_.get(), file.file_handle_.get());
        if (!contained.ok() || !contained.value()) {
            return path_failure<void>(
                ImagePathErrorCode::PathEscape,
                L"image_path.rename",
                L"renamed bundle file is no longer contained");
        }
        auto final_path = normalized_final_path(file.file_handle_.get());
        auto identity = query_windows_file_identity(file.file_handle_.get());
        if (!final_path.ok() || !identity.ok()) {
            const auto native_code = !final_path.ok()
                ? final_path.error().native_code
                : identity.error().native_code;
            return path_failure<void>(
                ImagePathErrorCode::IoError,
                L"image_path.rename",
                L"refreshing identity after rename failed",
                native_code);
        }
        if (!same_file_object(file.identity_, identity.value())) {
            return path_failure<void>(
                ImagePathErrorCode::IdentityMismatch,
                L"image_path.rename",
                L"rename changed the underlying file object");
        }
        file.final_path_ = final_path.move_value();
        file.identity_ = identity.move_value();
        file.relative_name_ = component.value().utf8_name;
        return Result<void>::success();
    } catch (const std::bad_alloc&) {
        return path_failure<void>(
            ImagePathErrorCode::IoError,
            L"image_path.rename",
            L"allocating rename information failed",
            ERROR_NOT_ENOUGH_MEMORY);
    }
}

Result<void> delete_validated_bundle_file(
    BundleWriterLock& writer_lock,
    ValidatedBundleFile& file) {
    if (writer_lock.state_ == nullptr) {
        return path_failure<void>(
            ImagePathErrorCode::WriterLockInvalid,
            L"image_path.delete",
            L"delete requires a live writer lock");
    }
    std::scoped_lock lock(writer_lock.state_->mutex);
    auto allowed = verify_cleanup_capability_locked(
        *writer_lock.state_,
        file.writer_authority_.lock(),
        file.deleted_,
        file.root_handle_.get(),
        file.file_handle_.get(),
        file.root_identity_,
        file.identity_);
    if (!allowed.ok()) {
        return allowed;
    }
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (!SetFileInformationByHandle(
            file.file_handle_.get(),
            FileDispositionInfo,
            &disposition,
            sizeof(disposition))) {
        const auto native_code = GetLastError();
        return path_failure<void>(
            native_open_error_code(native_code),
            L"image_path.delete",
            L"deleting the validated bundle file failed",
            native_code);
    }
    file.deleted_ = true;
    file.file_handle_.reset();
    return Result<void>::success();
}

} // namespace myrd::persistence
