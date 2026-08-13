#include "windows_volume.h"

#include <windows.h>
#include <winioctl.h>
#include <bcrypt.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <thread>
#include <vector>

namespace myrd {
namespace {
Error fail(const wchar_t* operation, const wchar_t* message, DWORD code = 0) {
    return {ErrorDomain::win32, code, operation, message, code};
}

constexpr DWORD kQueryCap = 64 * 1024;
constexpr DWORD kOutputCap = 64 * 1024;
constexpr GUID kBasicDataPartition{0xEBD0A0A2, 0xB9E5, 0x4433,
    {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};

std::wstring disk_path(std::uint32_t number);

bool valid_letter(wchar_t letter) noexcept {
    return letter >= L'A' && letter <= L'Z';
}

bool valid_timeout(std::chrono::milliseconds timeout) noexcept {
    return timeout > std::chrono::milliseconds::zero() && timeout <= std::chrono::seconds(120);
}

bool runtime_identity_ok(const m3::M3AimRuntimeInfo& info) {
    const auto address = aim::decode_device_address(info.device_id);
    return info.device_id.value != aim::kAllDevices && info.device_id.value != aim::kAutoDeviceNumber &&
        address.path_id == info.disk.path_id && address.target_id == info.disk.target_id && address.lun == info.disk.lun &&
        info.disk.path == disk_path(info.disk.physical_drive_number) && info.virtual_bytes != 0 && info.sector_size == 512 &&
        info.virtual_bytes == info.disk.length_bytes;
}
bool drive_mask_ok(DWORD mask, DWORD error) noexcept { return mask != 0 || error == ERROR_SUCCESS; }

bool native_geometry_ok(std::uint64_t bytes, std::uint32_t logical, std::uint32_t physical,
                        const m3::M3AimRuntimeInfo& info) noexcept {
    return bytes == info.virtual_bytes && logical == info.sector_size &&
        (physical == 512 || physical == 4096);
}
bool fixed_ioctl_sizes_ok(DWORD device_bytes, DWORD geometry_bytes, DWORD alignment_bytes,
                          DWORD layout_bytes, DWORD partition_count) noexcept {
    const auto header=offsetof(DRIVE_LAYOUT_INFORMATION_EX,PartitionEntry);
    return device_bytes >= sizeof(STORAGE_DEVICE_NUMBER) && geometry_bytes >= sizeof(DISK_GEOMETRY_EX) &&
        alignment_bytes >= sizeof(STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR) && layout_bytes >= header &&
        partition_count <= (layout_bytes-header)/sizeof(PARTITION_INFORMATION_EX);
}

bool layout_ok(const DRIVE_LAYOUT_INFORMATION_EX& layout, bool raw) noexcept {
    if (raw) return layout.PartitionStyle == PARTITION_STYLE_RAW && layout.PartitionCount == 0;
    return layout.PartitionStyle == PARTITION_STYLE_GPT && layout.PartitionCount == 1 &&
        layout.PartitionEntry[0].Gpt.PartitionType == kBasicDataPartition &&
        layout.PartitionEntry[0].PartitionLength.QuadPart > 0;
}

enum class ExtentClass { Invalid, Exact, Other, Mixed };
ExtentClass classify_extents(const std::byte* bytes, DWORD count, std::uint32_t target) noexcept {
    const auto header=offsetof(VOLUME_DISK_EXTENTS,Extents);
    if(!bytes || count<header+sizeof(DISK_EXTENT)) return ExtentClass::Invalid;
    const auto* value=reinterpret_cast<const VOLUME_DISK_EXTENTS*>(bytes);
    if(value->NumberOfDiskExtents==0 || value->NumberOfDiskExtents>(kQueryCap-header)/sizeof(DISK_EXTENT) || count<header+value->NumberOfDiskExtents*sizeof(DISK_EXTENT)) return ExtentClass::Invalid;
    if(value->NumberOfDiskExtents!=1) return ExtentClass::Mixed;
    return value->Extents[0].DiskNumber==target ? ExtentClass::Exact : ExtentClass::Other;
}
enum class MountClass { Absent, Exact, Occupied, Malformed };
MountClass classify_mount(wchar_t letter, const wchar_t* paths, std::size_t capacity, const std::wstring& volume) noexcept {
    (void)volume;
    if(!valid_letter(letter) || !paths || capacity<2) return MountClass::Malformed;
    const std::wstring wanted{letter,L':',L'\\'}; bool ended=false;
    for(std::size_t at=0; at<capacity;) { const wchar_t* p=paths+at; std::size_t n=0; while(at+n<capacity && p[n]) ++n; if(at+n==capacity) return MountClass::Malformed; if(n==0){ended=true;break;} if(_wcsicmp(p,wanted.c_str())==0) return MountClass::Exact; at+=n+1; }
    return ended ? MountClass::Absent : MountClass::Malformed;
}
bool owned_mount(const wchar_t* actual, const std::wstring& expected) noexcept {
    return actual && !expected.empty() && _wcsicmp(actual, expected.c_str()) == 0;
}
bool volume_device_path(const std::wstring& guid, std::wstring& device) {
    constexpr std::wstring_view prefix=L"\\\\?\\Volume{";
    if(!guid.starts_with(prefix) || guid.size()<=prefix.size()+2 || guid.back()!=L'\\' || guid.find(L'}')==std::wstring::npos) return false;
    device=guid.substr(0,guid.size()-1); return true;
}
Result<void> verify_exact_volume(HANDLE volume, std::uint32_t disk) {
    std::array<std::byte, kQueryCap> bytes{}; DWORD got{};
    if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
        bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr))
        return Result<void>::failure(fail(L"verify_extent", L"volume extent query failed", GetLastError()));
    if (classify_extents(bytes.data(), got, disk) != ExtentClass::Exact)
        return Result<void>::failure(fail(L"verify_extent", L"volume is not the exact runtime disk"));
    return Result<void>::success();
}
bool attribute_list_size_ok(BOOL first_result,DWORD error,SIZE_T size) noexcept { return !first_result && error==ERROR_INSUFFICIENT_BUFFER && size>0 && size<=64*1024; }

std::wstring disk_path(std::uint32_t number) {
    return L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
}

Result<HANDLE> open_disk(std::uint32_t number) {
    HANDLE handle = CreateFileW(disk_path(number).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return Result<HANDLE>::failure(fail(L"open_exact_physical_drive", L"CreateFileW failed", GetLastError()));
    return Result<HANDLE>::success(handle);
}

Result<void> validate_disk(const m3::M3AimRuntimeInfo& info, bool raw) {
    if (!runtime_identity_ok(info)) {
        return Result<void>::failure(fail(L"validate_exact_disk", L"runtime identity or geometry is ambiguous"));
    }
    auto opened = open_disk(info.disk.physical_drive_number);
    if (!opened.ok()) return Result<void>::failure(opened.error());
    HANDLE handle = opened.value();
    STORAGE_DEVICE_NUMBER device{}; DWORD device_returned{};
    const BOOL device_ok = DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER,
        nullptr, 0, &device, sizeof(device), &device_returned, nullptr);
    const DWORD device_error = GetLastError();
    DISK_GEOMETRY_EX geometry{}; DWORD geometry_returned{};
    const BOOL geometry_ok = DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
        nullptr, 0, &geometry, sizeof(geometry), &geometry_returned, nullptr);
    const DWORD geometry_error = GetLastError();
    STORAGE_PROPERTY_QUERY alignment_query{StorageAccessAlignmentProperty, PropertyStandardQuery, {}};
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{}; DWORD alignment_returned{};
    const BOOL alignment_ok = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY,
        &alignment_query, sizeof(alignment_query), &alignment, sizeof(alignment), &alignment_returned, nullptr);
    const DWORD alignment_error = GetLastError();
    std::array<std::byte, kQueryCap> layout_bytes{}; DWORD layout_returned{};
    const BOOL layout_query_ok = DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
        nullptr, 0, layout_bytes.data(), static_cast<DWORD>(layout_bytes.size()), &layout_returned, nullptr);
    const DWORD layout_error = GetLastError();
    CloseHandle(handle);
    if (!layout_query_ok && layout_error == ERROR_MORE_DATA)
        return Result<void>::failure(fail(L"validate_exact_disk", L"disk layout exceeds finite query cap", layout_error));
    if (!device_ok || device_returned < sizeof(device) || device.DeviceType!=FILE_DEVICE_DISK || device.PartitionNumber!=0) return Result<void>::failure(fail(L"IOCTL_STORAGE_GET_DEVICE_NUMBER", L"exact disk device-number query failed or truncated", device_error));
    if (!geometry_ok || geometry_returned < sizeof(geometry)) return Result<void>::failure(fail(L"IOCTL_DISK_GET_DRIVE_GEOMETRY_EX", L"exact disk geometry query failed or truncated", geometry_error));
    if (!alignment_ok || alignment_returned < sizeof(alignment)) return Result<void>::failure(fail(L"IOCTL_STORAGE_QUERY_PROPERTY", L"exact disk alignment query failed or truncated", alignment_error));
    const auto layout_header=offsetof(DRIVE_LAYOUT_INFORMATION_EX,PartitionEntry);
    if (!layout_query_ok || layout_returned < layout_header ||
        alignment.Version<sizeof(alignment) || alignment.Size<sizeof(alignment) || !native_geometry_ok(static_cast<std::uint64_t>(geometry.DiskSize.QuadPart), alignment.BytesPerLogicalSector, alignment.BytesPerPhysicalSector, info) ||
        device.DeviceNumber != info.disk.physical_drive_number)
        return Result<void>::failure(fail(L"validate_exact_disk", L"disk native facts did not match runtime identity", layout_error));
    auto* layout = reinterpret_cast<DRIVE_LAYOUT_INFORMATION_EX*>(layout_bytes.data());
    if (!fixed_ioctl_sizes_ok(device_returned,geometry_returned,alignment_returned,layout_returned,layout->PartitionCount) ||
        layout->PartitionCount > (kQueryCap-layout_header)/sizeof(PARTITION_INFORMATION_EX))
        return Result<void>::failure(fail(L"IOCTL_DISK_GET_DRIVE_LAYOUT_EX",L"disk layout count is truncated",layout_error));
    if (geometry.DiskSize.QuadPart != static_cast<LONGLONG>(info.virtual_bytes) ||
        geometry.Geometry.BytesPerSector != info.sector_size ||
        !layout_ok(*layout, raw)) {
        return Result<void>::failure(fail(L"validate_exact_disk", L"disk layout does not match required state"));
    }
    return Result<void>::success();
}

Result<ProductVolume> discover(const m3::M3AimRuntimeInfo& info, wchar_t letter) {
    std::array<wchar_t, MAX_PATH> name{};
    HANDLE find = FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size()));
    if (find == INVALID_HANDLE_VALUE) return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"FindFirstVolumeW failed", GetLastError()));
    std::vector<std::wstring> matches;
    do {
        std::array<std::byte, kQueryCap> bytes{}; DWORD got{};
        std::wstring device_path; if(!volume_device_path(name.data(),device_path)){FindVolumeClose(find);return Result<ProductVolume>::failure(fail(L"find_recovered_volume",L"malformed volume GUID path"));}
        HANDLE volume = CreateFileW(device_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, 0, nullptr);
        if (volume == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError(); FindVolumeClose(find);
            return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"opening enumerated volume failed", error));
        }
        {
            const BOOL ok = DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                nullptr, 0, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr);
            const DWORD extent_error = GetLastError();
            CloseHandle(volume);
            if (!ok) { FindVolumeClose(find); return Result<ProductVolume>::failure(fail(L"find_recovered_volume", extent_error == ERROR_MORE_DATA ? L"volume extents exceed finite query cap" : L"volume extent query failed", extent_error)); }
            if (got < offsetof(VOLUME_DISK_EXTENTS, Extents) + sizeof(DWORD)) { FindVolumeClose(find); return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"truncated volume extent query")); }
            const auto kind=classify_extents(bytes.data(),got,info.disk.physical_drive_number);
            if(kind==ExtentClass::Invalid) { FindVolumeClose(find); return Result<ProductVolume>::failure(fail(L"find_recovered_volume",L"truncated or ambiguous volume extents")); }
            if(kind==ExtentClass::Exact) matches.emplace_back(name.data());
            else if(kind==ExtentClass::Mixed) { FindVolumeClose(find); return Result<ProductVolume>::failure(fail(L"find_recovered_volume",L"mixed volume extents are refused")); }
        }
    } while (FindNextVolumeW(find, name.data(), static_cast<DWORD>(name.size())));
    const DWORD enumeration_error = GetLastError();
    FindVolumeClose(find);
    if (enumeration_error != ERROR_NO_MORE_FILES)
        return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"volume enumeration changed or failed", enumeration_error));
    if (matches.size() != 1) return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"volume identity is ambiguous"));
    DWORD serial{}; std::array<wchar_t, 256> filesystem{};
    if (!GetVolumeInformationW(matches[0].c_str(), nullptr, 0, &serial, nullptr, nullptr, filesystem.data(), static_cast<DWORD>(filesystem.size()))) {DWORD error=GetLastError();return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"GetVolumeInformationW failed", error));}
    if (wcscmp(filesystem.data(), L"NTFS") != 0) return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"exact volume is not NTFS"));
    std::array<wchar_t, kQueryCap / sizeof(wchar_t)> paths{}; DWORD needed{};
    if (!GetVolumePathNamesForVolumeNameW(matches[0].c_str(), paths.data(), static_cast<DWORD>(paths.size()), &needed)) {DWORD error=GetLastError();return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"mount path query failed",error));}
    if(needed>paths.size())return Result<ProductVolume>::failure(fail(L"find_recovered_volume",L"mount path query exceeded cap",ERROR_MORE_DATA));
    const auto mount=classify_mount(letter,paths.data(),paths.size(),matches[0]);
    if (mount==MountClass::Malformed) return Result<ProductVolume>::failure(fail(L"find_recovered_volume",L"malformed mount-path response"));
    SetLastError(ERROR_SUCCESS); const DWORD drive_mask=GetLogicalDrives(); const DWORD drive_error=GetLastError();
    if(!drive_mask_ok(drive_mask,drive_error)) return Result<ProductVolume>::failure(fail(L"find_recovered_volume",L"GetLogicalDrives failed",drive_error));
    if (mount!=MountClass::Exact && drive_mask & (1u << (letter - L'A')))
        return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"requested drive letter is occupied"));
    return Result<ProductVolume>::success({matches[0], info.disk.physical_drive_number, letter});
}

Result<void> rollback_exact_mount(std::uint32_t disk, wchar_t letter) {
    std::wstring mount{letter,L':',L'\\',L'\0'}; std::array<wchar_t,MAX_PATH> name{};
    if (!GetVolumeNameForVolumeMountPointW(mount.c_str(), name.data(), static_cast<DWORD>(name.size()))) {
        const DWORD error=GetLastError(); if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) return Result<void>::success();
        return Result<void>::failure(fail(L"rollback_provision_mount",L"mount-point query failed",error));
    }
    std::wstring device; if(!volume_device_path(name.data(),device))return Result<void>::failure(fail(L"rollback_provision_mount",L"malformed volume GUID path")); HANDLE volume=CreateFileW(device.c_str(),0,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr);
    if(volume==INVALID_HANDLE_VALUE) return Result<void>::failure(fail(L"rollback_provision_mount",L"opening mounted volume failed",GetLastError()));
    std::array<std::byte,kQueryCap> bytes{}; DWORD got{}; const BOOL ok=DeviceIoControl(volume,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr); const DWORD error=GetLastError(); CloseHandle(volume);
    const auto header=offsetof(VOLUME_DISK_EXTENTS,Extents);
    if(!ok || got<header+sizeof(DISK_EXTENT)) return Result<void>::failure(fail(L"rollback_provision_mount",L"mounted volume extents unavailable",error));
    auto* extents=reinterpret_cast<VOLUME_DISK_EXTENTS*>(bytes.data());
    if(extents->NumberOfDiskExtents!=1 || extents->Extents[0].DiskNumber!=disk) return Result<void>::failure(fail(L"rollback_provision_mount",L"mounted volume is not the exact provisioned disk"));
    if(!DeleteVolumeMountPointW(mount.c_str())) return Result<void>::failure(fail(L"rollback_provision_mount",L"removing provisioned mount failed",GetLastError()));
    return Result<void>::success();
}

Result<void> run_helper(std::uint32_t disk, wchar_t letter, std::chrono::milliseconds timeout) {
    if (!valid_letter(letter) || !valid_timeout(timeout)) return Result<void>::failure(fail(L"provision_new_volume", L"invalid helper arguments"));
    HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(10));
    HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
    const void* data = loaded ? LockResource(loaded) : nullptr;
    const DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
    if (!data || !size) return Result<void>::failure(fail(L"provision_new_volume", L"embedded provisioning resource unavailable", GetLastError()));
    wchar_t temp[MAX_PATH]{}; if (!GetTempPathW(MAX_PATH, temp)) return Result<void>::failure(fail(L"provision_new_volume", L"temporary script path failed", GetLastError()));
    std::wstring path; HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt != 8 && file == INVALID_HANDLE_VALUE; ++attempt) {
        std::array<std::byte, 16> random{};
        if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(random.data()), static_cast<ULONG>(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return Result<void>::failure(fail(L"provision_new_volume", L"temporary script random generation failed"));
        static constexpr wchar_t hex[] = L"0123456789abcdef"; std::wstring suffix;
        for (auto byte : random) { const auto value = static_cast<unsigned char>(byte); suffix += hex[value >> 4]; suffix += hex[value & 15]; }
        path = std::wstring(temp) + L"myrd-" + suffix + L".ps1";
        file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE && GetLastError() != ERROR_FILE_EXISTS) return Result<void>::failure(fail(L"provision_new_volume", L"temporary script creation failed", GetLastError()));
    }
    if (file == INVALID_HANDLE_VALUE) return Result<void>::failure(fail(L"provision_new_volume", L"temporary script creation failed", GetLastError()));
    DWORD written{}; const BOOL wrote = WriteFile(file, data, size, &written, nullptr); const DWORD write_error=GetLastError();
    if(!wrote || written!=size) { CloseHandle(file); DeleteFileW(path.c_str()); return Result<void>::failure(fail(L"provision_new_volume",L"temporary script write failed",write_error)); }
    const BOOL flushed = FlushFileBuffers(file); const DWORD flush_error=GetLastError();
    const BOOL closed = CloseHandle(file); const DWORD close_error=GetLastError();
    if(!flushed) { DeleteFileW(path.c_str()); return Result<void>::failure(fail(L"provision_new_volume",L"temporary script flush failed",flush_error)); }
    if(!closed) { DeleteFileW(path.c_str()); return Result<void>::failure(fail(L"provision_new_volume",L"temporary script close failed",close_error)); }
    std::wstring command = L"\"C:\\Windows\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" + path + L"\" -DiskNumber " + std::to_wstring(disk) + L" -DriveLetter " + letter;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE stdout_read{}, stdout_write{}, stderr_read{}, stderr_write{};
    if (!CreatePipe(&stdout_read, &stdout_write, &security, 0) || !CreatePipe(&stderr_read, &stderr_write, &security, 0) ||
        !SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) || !SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0)) {
        const DWORD error = GetLastError(); for (HANDLE h : {stdout_read,stdout_write,stderr_read,stderr_write}) if(h) CloseHandle(h); DeleteFileW(path.c_str());
        return Result<void>::failure(fail(L"provision_new_volume", L"bounded output pipe creation failed", error));
    }
    STARTUPINFOEXW startup{}; startup.StartupInfo.cb=sizeof(startup); startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = nullptr; startup.StartupInfo.hStdOutput = stdout_write; startup.StartupInfo.hStdError = stderr_write;
    SIZE_T attributes_size{}; SetLastError(ERROR_SUCCESS); const BOOL first_attributes=InitializeProcThreadAttributeList(nullptr,1,0,&attributes_size); const DWORD attribute_error=GetLastError();
    if(!attribute_list_size_ok(first_attributes,attribute_error,attributes_size)){CloseHandle(stdout_read);CloseHandle(stdout_write);CloseHandle(stderr_read);CloseHandle(stderr_write);DeleteFileW(path.c_str());return Result<void>::failure(fail(L"provision_new_volume",L"attribute-list sizing failed",attribute_error));}
    std::vector<std::byte> attributes(attributes_size);
    auto* list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
    HANDLE inherited[]{stdout_write,stderr_write};
    bool attribute_initialized=false;
    if(!InitializeProcThreadAttributeList(list,1,0,&attributes_size)) { DWORD error=GetLastError(); CloseHandle(stdout_read); CloseHandle(stdout_write); CloseHandle(stderr_read); CloseHandle(stderr_write); DeleteFileW(path.c_str()); return Result<void>::failure(fail(L"provision_new_volume",L"restricted child handle list initialization failed",error)); }
    attribute_initialized=true;
    if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr)) { DWORD error=GetLastError(); if(attribute_initialized)DeleteProcThreadAttributeList(list); CloseHandle(stdout_read); CloseHandle(stdout_write); CloseHandle(stderr_read); CloseHandle(stderr_write); DeleteFileW(path.c_str()); return Result<void>::failure(fail(L"provision_new_volume",L"restricted child handle list failed",error)); }
    startup.lpAttributeList=list;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW, nullptr, nullptr, &startup.StartupInfo, &process);
    const DWORD create_error=GetLastError(); if(attribute_initialized) DeleteProcThreadAttributeList(list);
    CloseHandle(stdout_write); CloseHandle(stderr_write);
    if (!created) { CloseHandle(stdout_read); CloseHandle(stderr_read); DeleteFileW(path.c_str()); return Result<void>::failure(fail(L"provision_new_volume", L"CreateProcessW failed", create_error)); }
    std::atomic<DWORD> stdout_bytes{}, stderr_bytes{}; std::atomic_bool pipe_error{}, producer_done{}, cancel_drains{};
    auto drain = [&](HANDLE h, std::atomic<DWORD>& total) { std::array<std::byte,4096> buffer{}; for (;;) { if(cancel_drains) return; DWORD available{}; if(!PeekNamedPipe(h,nullptr,0,nullptr,&available,nullptr)) { if(GetLastError()!=ERROR_BROKEN_PIPE) pipe_error=true; return; } if(available) { const DWORD chunk=(std::min)(available,static_cast<DWORD>(buffer.size())); DWORD read{}; if(!ReadFile(h,buffer.data(),chunk,&read,nullptr) || read!=chunk) { pipe_error=true; return; } if(total.fetch_add(read)+read>kOutputCap) { pipe_error=true; cancel_drains=true; return; } continue; } if(producer_done) return; std::this_thread::sleep_for(std::chrono::milliseconds(10)); } };
    std::thread stdout_thread(drain,stdout_read,std::ref(stdout_bytes)); std::thread stderr_thread(drain,stderr_read,std::ref(stderr_bytes));
    const DWORD wait = WaitForSingleObject(process.hProcess, static_cast<DWORD>(timeout.count()));
    bool termination_failed = false;
    bool child_exited = wait == WAIT_OBJECT_0;
    if (wait != WAIT_OBJECT_0) {
        if (!TerminateProcess(process.hProcess, wait == WAIT_TIMEOUT ? ERROR_TIMEOUT : ERROR_BUFFER_OVERFLOW) ||
            (child_exited = WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0) == false) {
            termination_failed = true;
            cancel_drains = true;
        }
    }
    producer_done = true;
    stdout_thread.join(); stderr_thread.join();
    CloseHandle(stdout_read); CloseHandle(stderr_read);
    DWORD exit_code{}; const BOOL exit_ok=GetExitCodeProcess(process.hProcess, &exit_code); const DWORD exit_error=GetLastError(); CloseHandle(process.hThread); CloseHandle(process.hProcess);
    if(!child_exited) return Result<void>::failure(fail(L"provision_new_volume",L"helper cleanup failed; temporary script retained",termination_failed ? ERROR_TIMEOUT : exit_error));
    const BOOL deleted=DeleteFileW(path.c_str()); const DWORD delete_error=GetLastError();
    if (!exit_ok || !deleted || pipe_error || termination_failed || wait != WAIT_OBJECT_0 || exit_code != 0) return Result<void>::failure(fail(L"provision_new_volume", L"provisioning helper failed", !deleted ? delete_error : exit_code));
    return Result<void>::success();
}
} // namespace

LockedVolume::LockedVolume(HANDLE handle, std::wstring identity, std::uint32_t disk) noexcept
    : handle_(handle), identity_(std::move(identity)), physical_drive_number_(disk) {}
LockedVolume::LockedVolume(LockedVolume&& other) noexcept
    : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)), identity_(std::move(other.identity_)),
      physical_drive_number_(other.physical_drive_number_) {}
LockedVolume& LockedVolume::operator=(LockedVolume&& other) noexcept {
    if (this != &other) { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); handle_ = std::exchange(other.handle_, INVALID_HANDLE_VALUE); identity_ = std::move(other.identity_); physical_drive_number_ = other.physical_drive_number_; }
    return *this;
}
LockedVolume::~LockedVolume() noexcept { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
Result<void> LockedVolume::flush() {
    if (!FlushFileBuffers(handle_)) return Result<void>::failure(fail(L"flush", L"FlushFileBuffers failed", GetLastError()));
    return Result<void>::success();
}
Result<void> LockedVolume::dismount() {
    DWORD ignored{};
    if (!DeviceIoControl(handle_, FSCTL_DISMOUNT_VOLUME, nullptr, 0, nullptr, 0, &ignored, nullptr))
        return Result<void>::failure(fail(L"dismount", L"FSCTL_DISMOUNT_VOLUME failed", GetLastError()));
    return Result<void>::success();
}
bool volume_lock_busy_error(DWORD error) noexcept { return error == ERROR_SHARING_VIOLATION || error == ERROR_LOCK_VIOLATION || error == ERROR_BUSY; }
Result<VolumeLockAttempt> lock_volume(const ProductVolume& product) {
    std::wstring path;
    if (!volume_device_path(product.volume_guid_path, path)) return Result<VolumeLockAttempt>::failure(fail(L"lock", L"malformed volume GUID path"));
    HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { const DWORD error = GetLastError(); if (volume_lock_busy_error(error)) return Result<VolumeLockAttempt>::success({true, std::nullopt}); return Result<VolumeLockAttempt>::failure(fail(L"lock", L"opening exact volume failed", error)); }
    DWORD ignored{};
    if (!DeviceIoControl(handle, FSCTL_LOCK_VOLUME, nullptr, 0, nullptr, 0, &ignored, nullptr)) { const DWORD error=GetLastError(); CloseHandle(handle); if (volume_lock_busy_error(error)) return Result<VolumeLockAttempt>::success({true, std::nullopt}); return Result<VolumeLockAttempt>::failure(fail(L"lock", L"FSCTL_LOCK_VOLUME failed", error)); }
    const auto exact = verify_exact_volume(handle, product.physical_drive_number);
    if (!exact.ok()) { CloseHandle(handle); return Result<VolumeLockAttempt>::failure(exact.error()); }
    LockedVolume locked(handle, product.volume_guid_path, product.physical_drive_number);
    return Result<VolumeLockAttempt>::success({false, std::optional<LockedVolume>(std::move(locked))});
}

Result<ProductVolume> provision_new_volume(const m3::M3AimRuntimeInfo& info, wchar_t letter, std::chrono::milliseconds timeout) {
    if (!valid_letter(letter)) return Result<ProductVolume>::failure(fail(L"provision_new_volume", L"invalid drive letter"));
    auto checked = validate_disk(info, true); if (!checked.ok()) return Result<ProductVolume>::failure(checked.error());
    SetLastError(ERROR_SUCCESS); const DWORD drive_mask=GetLogicalDrives(); const DWORD drive_error=GetLastError(); if(!drive_mask_ok(drive_mask,drive_error))return Result<ProductVolume>::failure(fail(L"provision_new_volume",L"GetLogicalDrives failed",drive_error));
    if (drive_mask & (1u << (letter - L'A'))) return Result<ProductVolume>::failure(fail(L"provision_new_volume", L"requested drive letter is occupied"));
    auto helper = run_helper(info.disk.physical_drive_number, letter, timeout); if (!helper.ok()) { auto rollback=rollback_exact_mount(info.disk.physical_drive_number,letter); if(!rollback.ok()) return Result<ProductVolume>::failure(rollback.error()); return Result<ProductVolume>::failure(helper.error()); }
    auto found=find_recovered_volume(info, letter, timeout); if(!found.ok()) { auto rollback=rollback_exact_mount(info.disk.physical_drive_number,letter); if(!rollback.ok()) return Result<ProductVolume>::failure(rollback.error()); } return found;
}
Result<ProductVolume> find_recovered_volume(const m3::M3AimRuntimeInfo& info, wchar_t letter, std::chrono::milliseconds timeout) {
    if (!valid_letter(letter)) return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"invalid drive letter"));
    if (timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(120)) return Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"invalid discovery timeout"));
    auto checked = validate_disk(info, false); if (!checked.ok()) return Result<ProductVolume>::failure(checked.error());
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    Result<ProductVolume> last = Result<ProductVolume>::failure(fail(L"find_recovered_volume", L"volume discovery timed out"));
    do {
        last = discover(info, letter);
        if (last.ok()) return last;
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (true);
    return last;
}
Result<void> flush_volume(const ProductVolume& volume) { std::wstring device;if(!volume_device_path(volume.volume_guid_path,device))return Result<void>::failure(fail(L"flush_volume",L"malformed volume GUID path")); HANDLE h=CreateFileW(device.c_str(), GENERIC_READ|GENERIC_WRITE, FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,0,nullptr); if(h==INVALID_HANDLE_VALUE)return Result<void>::failure(fail(L"flush_volume",L"open volume failed",GetLastError())); std::array<std::byte,kQueryCap> bytes{}; DWORD got{}; BOOL queried=DeviceIoControl(h,IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,nullptr,0,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr); DWORD query_error=GetLastError(); auto kind=queried?classify_extents(bytes.data(),got,volume.physical_drive_number):ExtentClass::Invalid; if(kind!=ExtentClass::Exact){CloseHandle(h);return Result<void>::failure(fail(L"flush_volume",L"exact volume extent validation failed",query_error));} BOOL ok=FlushFileBuffers(h); DWORD e=GetLastError(); CloseHandle(h); return ok?Result<void>::success():Result<void>::failure(fail(L"flush_volume",L"FlushFileBuffers failed",e)); }
Result<void> assign_drive_letter(ProductVolume& volume) { if (!valid_letter(volume.drive_letter)) return Result<void>::failure(fail(L"assign_drive_letter",L"invalid drive letter")); std::wstring mount{volume.drive_letter,L':',L'\\',L'\0'}; std::array<wchar_t, MAX_PATH> current{}; if (GetVolumeNameForVolumeMountPointW(mount.c_str(),current.data(),static_cast<DWORD>(current.size()))) { if (_wcsicmp(current.data(),volume.volume_guid_path.c_str())==0) return Result<void>::success(); return Result<void>::failure(fail(L"assign_drive_letter",L"requested drive letter is occupied")); } const DWORD query_error=GetLastError(); if (query_error!=ERROR_FILE_NOT_FOUND && query_error!=ERROR_PATH_NOT_FOUND) return Result<void>::failure(fail(L"assign_drive_letter",L"mount-point query failed",query_error)); if (SetVolumeMountPointW(mount.c_str(),volume.volume_guid_path.c_str())) return Result<void>::success(); return Result<void>::failure(fail(L"assign_drive_letter",L"SetVolumeMountPointW failed",GetLastError())); }
Result<void> remove_drive_letter(const ProductVolume& volume) { if (!valid_letter(volume.drive_letter)) return Result<void>::failure(fail(L"remove_drive_letter",L"invalid drive letter")); std::wstring mount{volume.drive_letter,L':',L'\\',L'\0'}; std::array<wchar_t,MAX_PATH> actual{}; if(!GetVolumeNameForVolumeMountPointW(mount.c_str(),actual.data(),static_cast<DWORD>(actual.size()))){DWORD e=GetLastError();if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return Result<void>::success();return Result<void>::failure(fail(L"remove_drive_letter",L"mount ownership query failed",e));} if(!owned_mount(actual.data(),volume.volume_guid_path))return Result<void>::failure(fail(L"remove_drive_letter",L"refusing to remove foreign mount")); if(DeleteVolumeMountPointW(mount.c_str()))return Result<void>::success();DWORD e=GetLastError();if(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND)return Result<void>::success();return Result<void>::failure(fail(L"remove_drive_letter",L"DeleteVolumeMountPointW failed",e)); }
} // namespace myrd
