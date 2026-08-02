#include "m1_raw_protocol_io.h"

#include "src/common/sha256.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <ntddscsi.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <malloc.h>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace myrd {

namespace {

constexpr std::uint64_t kTestOffset = 8ull * 1024ull * 1024ull; // 8 MiB
constexpr std::uint64_t kTestLength = 1ull * 1024ull * 1024ull; // 1 MiB
constexpr std::uint64_t kRawIoAlignment = 4096;

constexpr auto kCounterTimeout = std::chrono::seconds(5);

Result<void> wait_for_counter(
    const std::function<proxy::RequestCounters()>& counters,
    std::uint64_t proxy::RequestCounters::*member,
    std::uint64_t baseline,
    const wchar_t* operation) {
    const auto deadline = std::chrono::steady_clock::now() + kCounterTimeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (counters().*member > baseline) {
            return Result<void>::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return Result<void>::failure({
        ErrorDomain::evidence, 0, operation,
        L"provider request counter did not increase within five seconds"});
}

Result<void> require_all_zero(
    std::span<const std::byte> bytes, const wchar_t* operation) {
    if (!std::all_of(bytes.begin(), bytes.end(),
                     [](std::byte value) { return value == std::byte{0}; })) {
        return Result<void>::failure({
            ErrorDomain::evidence, 0, operation,
            L"raw disk readback was not all zero"});
    }
    return Result<void>::success();
}

Error win32_error(const wchar_t* operation, const wchar_t* message) {
    return {ErrorDomain::win32,
            static_cast<std::uint32_t>(GetLastError()),
            operation, message};
}

using AlignedBuffer = std::unique_ptr<std::byte, decltype(&_aligned_free)>;

Result<AlignedBuffer> allocate_raw_io_buffer(std::size_t size) {
    if (size == 0 || (size % kRawIoAlignment) != 0) {
        return Result<AlignedBuffer>::failure({
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::misaligned),
            L"allocate_raw_io_buffer", L"raw I/O size is not 4096-byte aligned"});
    }
    AlignedBuffer buffer(
        static_cast<std::byte*>(_aligned_malloc(size, kRawIoAlignment)),
        &_aligned_free);
    if (!buffer) {
        return Result<AlignedBuffer>::failure({
            ErrorDomain::storage, ERROR_NOT_ENOUGH_MEMORY,
            L"_aligned_malloc(raw I/O)", L"failed to allocate aligned raw I/O buffer"});
    }
    return Result<AlignedBuffer>::success(std::move(buffer));
}

Result<void> validate_raw_io_range(
    std::uint64_t offset, std::size_t length, const wchar_t* operation) {
    if ((offset % kRawIoAlignment) != 0 ||
        (length % kRawIoAlignment) != 0 || length == 0) {
        return Result<void>::failure({
            ErrorDomain::protocol,
            static_cast<std::uint32_t>(OperationErrorCode::misaligned),
            operation, L"raw I/O offset and length must be 4096-byte aligned"});
    }
    return Result<void>::success();
}

Result<void> verify_handle_identity(
    HANDLE handle, const aim::LocatedDisk& disk,
    const wchar_t* operation) {
    GET_LENGTH_INFORMATION length{};
    STORAGE_DEVICE_NUMBER number{};
    SCSI_ADDRESS address{};
    DWORD bytes = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO,
            nullptr, 0, &length, sizeof(length), &bytes, nullptr)) {
        return Result<void>::failure(
            win32_error(L"IOCTL_DISK_GET_LENGTH_INFO", operation));
    }
    if (!DeviceIoControl(handle, IOCTL_STORAGE_GET_DEVICE_NUMBER,
            nullptr, 0, &number, sizeof(number), &bytes, nullptr)) {
        return Result<void>::failure(
            win32_error(L"IOCTL_STORAGE_GET_DEVICE_NUMBER", operation));
    }
    if (!DeviceIoControl(handle, IOCTL_SCSI_GET_ADDRESS,
            nullptr, 0, &address, sizeof(address), &bytes, nullptr)) {
        return Result<void>::failure(
            win32_error(L"IOCTL_SCSI_GET_ADDRESS", operation));
    }
    if (static_cast<std::uint64_t>(length.Length.QuadPart) !=
            disk.length_bytes ||
        number.DeviceNumber != disk.physical_drive_number ||
        address.PathId != disk.path_id ||
        address.TargetId != disk.target_id ||
        address.Lun != disk.lun) {
        return Result<void>::failure({
            ErrorDomain::evidence, 0, L"verify_raw_disk_identity",
            L"disk number, SCSI address, or length changed before raw I/O"});
    }
    return Result<void>::success();
}

} // anonymous namespace

Result<void> WindowsM1RawDiskApi::open(const aim::LocatedDisk& disk) {
    if (handle_) {
        return Result<void>::failure({
            ErrorDomain::protocol, 0, L"WindowsM1RawDiskApi::open",
            L"raw disk is already open"});
    }
    const auto expected_path = std::wstring(L"\\\\.\\PhysicalDrive") +
        std::to_wstring(disk.physical_drive_number);
    if (disk.path != expected_path) {
        return Result<void>::failure({
            ErrorDomain::evidence, 0, L"WindowsM1RawDiskApi::open(path)",
            L"located disk path does not match PhysicalDrive number"});
    }

    UniqueHandle read_handle(CreateFileW(
        disk.path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!read_handle) {
        return Result<void>::failure(
            win32_error(L"CreateFileW(raw identity)",
                        L"failed to open raw disk for identity check"));
    }
    auto read_identity = verify_handle_identity(
        read_handle.get(), disk, L"read identity check failed");
    if (!read_identity) return read_identity;

    handle_ = UniqueHandle(CreateFileW(
        disk.path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_WRITE_THROUGH,
        nullptr));
    if (!handle_) {
        return Result<void>::failure(
            win32_error(L"CreateFileW(raw write)",
                        L"failed to open raw disk for write"));
    }
    auto write_identity = verify_handle_identity(
        handle_.get(), disk, L"write identity check failed");
    if (!write_identity) return write_identity;

    GET_DISK_ATTRIBUTES attributes{};
    DWORD bytes = 0;
    if (!DeviceIoControl(handle_.get(), IOCTL_DISK_GET_DISK_ATTRIBUTES,
            nullptr, 0, &attributes, sizeof(attributes), &bytes, nullptr)) {
        return Result<void>::failure(
            win32_error(L"IOCTL_DISK_GET_DISK_ATTRIBUTES", L"failed"));
    }
    if ((attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0) {
        return Result<void>::failure({
            ErrorDomain::win32, ERROR_NOT_READY,
            L"IOCTL_DISK_GET_DISK_ATTRIBUTES",
            L"created raw disk is offline"});
    }
    if ((attributes.Attributes & DISK_ATTRIBUTE_READ_ONLY) != 0) {
        SET_DISK_ATTRIBUTES update{};
        update.Version = sizeof(update);
        update.Persist = FALSE;
        update.Attributes = 0;
        update.AttributesMask = DISK_ATTRIBUTE_READ_ONLY;
        if (!DeviceIoControl(handle_.get(), IOCTL_DISK_SET_DISK_ATTRIBUTES,
                &update, sizeof(update), nullptr, 0, &bytes, nullptr)) {
            return Result<void>::failure(
                win32_error(L"IOCTL_DISK_SET_DISK_ATTRIBUTES",
                            L"failed to clear non-persistent read-only state"));
        }
        attributes = {};
        if (!DeviceIoControl(handle_.get(), IOCTL_DISK_GET_DISK_ATTRIBUTES,
                nullptr, 0, &attributes, sizeof(attributes), &bytes, nullptr)) {
            return Result<void>::failure(
                win32_error(L"IOCTL_DISK_GET_DISK_ATTRIBUTES(after_set)",
                            L"failed to verify writable attributes"));
        }
    }
    if ((attributes.Attributes &
            (DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY)) != 0) {
        return Result<void>::failure({
            ErrorDomain::win32, ERROR_WRITE_PROTECT,
            L"IOCTL_DISK_GET_DISK_ATTRIBUTES(after_set)",
            L"created raw disk remains offline or read-only"});
    }

    SetLastError(ERROR_SUCCESS);
    const BOOL writable = DeviceIoControl(
        handle_.get(), IOCTL_DISK_IS_WRITABLE,
        nullptr, 0, nullptr, 0, &bytes, nullptr);
    if (!writable) {
        const auto code = static_cast<std::uint32_t>(GetLastError());
        return Result<void>::failure({
            ErrorDomain::win32,
            code == ERROR_SUCCESS
                ? static_cast<std::uint32_t>(ERROR_WRITE_PROTECT) : code,
            L"IOCTL_DISK_IS_WRITABLE", L"disk stack rejected raw writes"});
    }

    return Result<void>::success();
}

Result<void> WindowsM1RawDiskApi::seek(std::uint64_t offset) {
    if (!handle_) {
        return Result<void>::failure({
            ErrorDomain::protocol, 0, L"WindowsM1RawDiskApi::seek",
            L"raw disk is not open"});
    }
    if (offset > static_cast<std::uint64_t>(
                     std::numeric_limits<LONGLONG>::max())) {
        return Result<void>::failure({
            ErrorDomain::protocol, 0, L"WindowsM1RawDiskApi::seek",
            L"offset cannot be represented by SetFilePointerEx"});
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle_.get(), position, nullptr, FILE_BEGIN)) {
        return Result<void>::failure(
            win32_error(L"SetFilePointerEx", L"raw disk seek failed"));
    }
    return Result<void>::success();
}

Result<void> WindowsM1RawDiskApi::read(
    std::uint64_t offset, std::span<std::byte> buffer) {
    if (buffer.size() > std::numeric_limits<DWORD>::max()) {
        return Result<void>::failure({
            ErrorDomain::protocol, 0, L"ReadFile(raw disk)",
            L"read length exceeds Win32 limit"});
    }
    auto valid = validate_raw_io_range(offset, buffer.size(), L"ReadFile(raw disk)");
    if (!valid) return valid;
    auto aligned = allocate_raw_io_buffer(buffer.size());
    if (!aligned) return Result<void>::failure(aligned.error());
    std::memset(aligned.value().get(), 0xA5, buffer.size());

    auto positioned = seek(offset);
    if (!positioned) return positioned;
    DWORD bytes_read = 0;
    if (!ReadFile(handle_.get(), aligned.value().get(),
                  static_cast<DWORD>(buffer.size()),
                  &bytes_read, nullptr)) {
        return Result<void>::failure(
            win32_error(L"ReadFile(raw disk)", L"raw disk read failed"));
    }
    if (bytes_read != buffer.size()) {
        return Result<void>::failure({
            ErrorDomain::storage, 0, L"ReadFile(raw disk)", L"short read"});
    }
    std::memcpy(buffer.data(), aligned.value().get(), buffer.size());
    return Result<void>::success();
}

Result<void> WindowsM1RawDiskApi::write(
    std::uint64_t offset, std::span<const std::byte> buffer) {
    if (buffer.size() > std::numeric_limits<DWORD>::max()) {
        return Result<void>::failure({
            ErrorDomain::protocol, 0, L"WriteFile(raw disk)",
            L"write length exceeds Win32 limit"});
    }
    auto valid = validate_raw_io_range(offset, buffer.size(), L"WriteFile(raw disk)");
    if (!valid) return valid;
    auto aligned = allocate_raw_io_buffer(buffer.size());
    if (!aligned) return Result<void>::failure(aligned.error());
    std::memcpy(aligned.value().get(), buffer.data(), buffer.size());
    auto positioned = seek(offset);
    if (!positioned) return positioned;
    DWORD bytes_written = 0;
    if (!WriteFile(handle_.get(), aligned.value().get(),
                   static_cast<DWORD>(buffer.size()),
                   &bytes_written, nullptr)) {
        return Result<void>::failure(
            win32_error(L"WriteFile(raw disk)", L"raw disk write failed"));
    }
    if (bytes_written != buffer.size()) {
        return Result<void>::failure({
            ErrorDomain::storage, 0, L"WriteFile(raw disk)", L"short write"});
    }
    return Result<void>::success();
}

Result<void> WindowsM1RawDiskApi::flush() {
    if (!handle_) {
        return Result<void>::failure({
            ErrorDomain::protocol, 0, L"FlushFileBuffers(raw disk)",
            L"raw disk is not open"});
    }
    if (!FlushFileBuffers(handle_.get())) {
        return Result<void>::failure(
            win32_error(L"FlushFileBuffers(raw disk)",
                        L"raw disk flush failed"));
    }
    return Result<void>::success();
}

Result<void> WindowsM1RawDiskApi::trim_range(
    std::uint64_t offset, std::uint64_t length) {
    if (!handle_) {
        return Result<void>::failure({
            ErrorDomain::protocol, 0, L"DeviceIoControl(TRIM)",
            L"raw disk is not open"});
    }
    struct TrimInput {
        DEVICE_MANAGE_DATA_SET_ATTRIBUTES attributes;
        DEVICE_DATA_SET_RANGE range;
    };
    TrimInput input{};
    input.attributes.Size = sizeof(input.attributes);
    input.attributes.Action = DeviceDsmAction_Trim;
    // This verifier operates on a raw PhysicalDrive range that is not owned by
    // a file system.  Tell the storage stack that it does not need to validate
    // file allocation before translating DSM TRIM to SCSI UNMAP.
    input.attributes.Flags = DEVICE_DSM_FLAG_TRIM_NOT_FS_ALLOCATED;
    input.attributes.DataSetRangesOffset = offsetof(TrimInput, range);
    input.attributes.DataSetRangesLength = sizeof(input.range);
    input.range.StartingOffset = offset;
    input.range.LengthInBytes = length;

    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle_.get(),
            IOCTL_STORAGE_MANAGE_DATA_SET_ATTRIBUTES,
            &input, sizeof(input), nullptr, 0, &bytes_returned, nullptr)) {
        return Result<void>::failure(
            win32_error(L"DeviceIoControl(TRIM)", L"raw disk trim failed"));
    }
    if (!FlushFileBuffers(handle_.get())) {
        return Result<void>::failure(
            win32_error(L"FlushFileBuffers(after TRIM)",
                        L"failed to drain the raw disk after trim"));
    }
    if (!DeviceIoControl(handle_.get(), IOCTL_DISK_UPDATE_PROPERTIES,
            nullptr, 0, nullptr, 0, &bytes_returned, nullptr)) {
        return Result<void>::failure(
            win32_error(L"IOCTL_DISK_UPDATE_PROPERTIES(after TRIM)",
                        L"failed to refresh disk properties after trim"));
    }

    return Result<void>::success();
}

Result<M1RawProtocolEvidence> verify_m1_raw_protocol(
    IM1RawDiskApi& api,
    const aim::LocatedDisk& disk,
    const proxy::RequestCounters& before,
    const std::function<proxy::RequestCounters()>& counters) {

    const auto expected_path = std::wstring(L"\\\\.\\PhysicalDrive") +
        std::to_wstring(disk.physical_drive_number);
    if (disk.path != expected_path) {
        return Result<M1RawProtocolEvidence>::failure({
            ErrorDomain::evidence, 0, L"verify_m1_raw_protocol(path)",
            L"located disk path does not match its PhysicalDrive number"});
    }
    if (disk.length_bytes < kTestOffset + kTestLength ||
        (kTestOffset % 4096) != 0 || (kTestLength % 4096) != 0) {
        return Result<M1RawProtocolEvidence>::failure({
            ErrorDomain::evidence, 0, L"verify_m1_raw_protocol(range)",
            L"verification range is outside the disk or not 4096-byte aligned"});
    }

    auto opened = api.open(disk);
    if (!opened) {
        return Result<M1RawProtocolEvidence>::failure(opened.error());
    }

    M1RawProtocolEvidence evidence{};
    evidence.offset = kTestOffset;
    evidence.length = kTestLength;
    evidence.zero_write_verified = false;
    evidence.unmap_verified = false;
    evidence.flush_succeeded = false;

    // 1. Write deterministic nonzero pattern
    std::vector<std::byte> pattern(static_cast<std::size_t>(kTestLength));
    std::byte val{0x5A};
    for (auto& b : pattern) {
        b = val;
        val = static_cast<std::byte>(static_cast<int>(val) + 1);
    }
    auto write_hash = sha256_hex(pattern);
    if (!write_hash) {
        return Result<M1RawProtocolEvidence>::failure(write_hash.error());
    }
    evidence.write_sha256 = write_hash.value();
    auto w = api.write(kTestOffset, pattern);
    if (!w) {
        return Result<M1RawProtocolEvidence>::failure(w.error());
    }

    // 2. Flush and read back
    auto f = api.flush();
    if (!f) return Result<M1RawProtocolEvidence>::failure(f.error());

    std::vector<std::byte> readback(static_cast<std::size_t>(kTestLength));
    auto r = api.read(kTestOffset, readback);
    if (!r) {
        return Result<M1RawProtocolEvidence>::failure(r.error());
    }
    auto read_hash = sha256_hex(readback);
    if (!read_hash) {
        return Result<M1RawProtocolEvidence>::failure(read_hash.error());
    }
    evidence.read_sha256 = read_hash.value();
    if (readback != pattern || evidence.read_sha256 != evidence.write_sha256) {
        return Result<M1RawProtocolEvidence>::failure({
            ErrorDomain::evidence, 0, L"verify_m1_raw_protocol(pattern)",
            L"raw disk pattern readback did not match the write"});
    }

    // 3. All-zero write (triggers ZERO code 7)
    std::vector<std::byte> zeros(static_cast<std::size_t>(kTestLength),
                                  std::byte{0});
    auto zw = api.write(kTestOffset, zeros);
    if (!zw) {
        return Result<M1RawProtocolEvidence>::failure(zw.error());
    }
    auto zero_flush = api.flush();
    if (!zero_flush) {
        return Result<M1RawProtocolEvidence>::failure(zero_flush.error());
    }

    auto zero_counter = wait_for_counter(
        counters, &proxy::RequestCounters::zero, before.zero,
        L"wait_for_counter(zero)");
    if (!zero_counter) {
        return Result<M1RawProtocolEvidence>::failure(zero_counter.error());
    }
    evidence.zero_write_verified = true;

    // Verify zeros read back
    auto zr = api.read(kTestOffset, readback);
    if (!zr) {
        return Result<M1RawProtocolEvidence>::failure(zr.error());
    }
    auto zero_readback = require_all_zero(
        readback, L"verify_m1_raw_protocol(zero_readback)");
    if (!zero_readback) {
        return Result<M1RawProtocolEvidence>::failure(zero_readback.error());
    }

    // 4. Rewrite pattern
    auto rw = api.write(kTestOffset, pattern);
    if (!rw) {
        return Result<M1RawProtocolEvidence>::failure(rw.error());
    }
    auto rewrite_flush = api.flush();
    if (!rewrite_flush) {
        return Result<M1RawProtocolEvidence>::failure(rewrite_flush.error());
    }

    // 5. Issue TRIM/UNMAP
    auto trim_result = api.trim_range(kTestOffset, kTestLength);
    if (!trim_result) {
        return Result<M1RawProtocolEvidence>::failure(trim_result.error());
    }

    auto unmap_counter = wait_for_counter(
        counters, &proxy::RequestCounters::unmap, before.unmap,
        L"wait_for_counter(unmap)");
    if (!unmap_counter) {
        return Result<M1RawProtocolEvidence>::failure(unmap_counter.error());
    }
    evidence.unmap_verified = true;

    // Verify zeros after trim
    auto tr = api.read(kTestOffset, readback);
    if (!tr) {
        return Result<M1RawProtocolEvidence>::failure(tr.error());
    }
    if (readback == pattern) {
        return Result<M1RawProtocolEvidence>::failure({
            ErrorDomain::evidence, 0,
            L"verify_m1_raw_protocol(trim_readback)",
            L"raw disk readback returned the pre-TRIM pattern"});
    }
    if (std::all_of(readback.begin(), readback.end(),
                    [](std::byte value) { return value == std::byte{0xA5}; })) {
        return Result<M1RawProtocolEvidence>::failure({
            ErrorDomain::evidence, 0,
            L"verify_m1_raw_protocol(trim_readback)",
            L"raw disk read path did not overwrite the sentinel buffer"});
    }
    auto trim_readback = require_all_zero(
        readback, L"verify_m1_raw_protocol(trim_readback)");
    if (!trim_readback) {
        return Result<M1RawProtocolEvidence>::failure(trim_readback.error());
    }

    evidence.flush_succeeded = true;

    return Result<M1RawProtocolEvidence>::success(evidence);
}

} // namespace myrd
