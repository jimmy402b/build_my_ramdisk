#include "file_identity.h"

#include <cstring>
#include <utility>

namespace myrd::persistence {

namespace {

template <class T>
[[nodiscard]] Result<T> identity_failure(
    const std::uint32_t native_code,
    const wchar_t* message) {
    return Result<T>::failure({
        ErrorDomain::win32,
        native_code,
        L"file_identity.query",
        message,
        native_code,
    });
}

} // namespace

bool same_file_object(
    const WindowsFileIdentity& left,
    const WindowsFileIdentity& right) noexcept {
    return left.volume_serial_number == right.volume_serial_number &&
        left.file_id == right.file_id;
}

Result<WindowsFileIdentity> query_windows_file_identity(HANDLE handle) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        return identity_failure<WindowsFileIdentity>(
            ERROR_INVALID_HANDLE,
            L"file identity requires a valid open handle");
    }

    FILE_ID_INFO file_id_info{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileIdInfo,
            &file_id_info,
            sizeof(file_id_info))) {
        return identity_failure<WindowsFileIdentity>(
            GetLastError(),
            L"FileIdInfo query failed");
    }

    FILE_STANDARD_INFO standard_info{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileStandardInfo,
            &standard_info,
            sizeof(standard_info))) {
        return identity_failure<WindowsFileIdentity>(
            GetLastError(),
            L"FileStandardInfo query failed");
    }
    if (standard_info.EndOfFile.QuadPart < 0) {
        return identity_failure<WindowsFileIdentity>(
            ERROR_INVALID_DATA,
            L"file reported a negative length");
    }

    FILE_ATTRIBUTE_TAG_INFO attribute_info{};
    if (!GetFileInformationByHandleEx(
            handle,
            FileAttributeTagInfo,
            &attribute_info,
            sizeof(attribute_info))) {
        return identity_failure<WindowsFileIdentity>(
            GetLastError(),
            L"FileAttributeTagInfo query failed");
    }

    WindowsFileIdentity identity{};
    identity.volume_serial_number = file_id_info.VolumeSerialNumber;
    static_assert(sizeof(file_id_info.FileId.Identifier) ==
        sizeof(identity.file_id));
    std::memcpy(
        identity.file_id.data(),
        file_id_info.FileId.Identifier,
        identity.file_id.size());
    identity.size = static_cast<std::uint64_t>(
        standard_info.EndOfFile.QuadPart);
    identity.attributes = attribute_info.FileAttributes;
    identity.reparse_tag = attribute_info.ReparseTag;
    identity.hard_link_count = standard_info.NumberOfLinks;
    return Result<WindowsFileIdentity>::success(std::move(identity));
}

} // namespace myrd::persistence
