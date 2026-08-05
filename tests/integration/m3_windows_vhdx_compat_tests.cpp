#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#include <virtdisk.h>

#include "src/common/unique_handle.h"
#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/windows_srb_transport.h"
#include "src/persistence/image_paths.h"
#include "src/persistence/vhdx_format.h"
#include "src/persistence/vhdx_reader.h"
#include "src/persistence/vhdx_writer.h"
#include "src/persistence/writer_lock.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using myrd::OperationContext;
using myrd::UniqueHandle;
using namespace myrd::persistence;

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = 1024 * kKiB;
constexpr int kBlockedExitCode = 77;
constexpr GUID kMicrosoftVirtualStorageVendor = {
    0xec984aec, 0xa0f9, 0x47e9,
    {0x90, 0x1f, 0x71, 0x41, 0x5a, 0x66, 0x34, 0x5b}};

struct CaseReport final {
    std::string name;
    std::string status{"FAIL"};
    std::string operation;
    std::string detail;
    std::uint32_t win32_code{};
    bool cleanup_attempted{};
    bool cleanup_succeeded{};
    std::uint32_t cleanup_win32{};
    std::string recovery_command;
};

struct GateReport final {
    std::string status{"FAIL"};
    std::string reason;
    bool elevated{};
    std::int64_t aim_devices_online{-1};
    std::vector<CaseReport> cases;
};

class GateFailure final : public std::runtime_error {
public:
    GateFailure(std::string operation, const std::uint32_t code,
                std::string detail)
        : std::runtime_error(operation + ": " + detail),
          operation_(std::move(operation)), code_(code),
          detail_(std::move(detail)) {}

    [[nodiscard]] const std::string& operation() const noexcept {
        return operation_;
    }
    [[nodiscard]] std::uint32_t code() const noexcept { return code_; }
    [[nodiscard]] const std::string& detail() const noexcept {
        return detail_;
    }

private:
    std::string operation_;
    std::uint32_t code_{};
    std::string detail_;
};

class GateBlocked final : public std::runtime_error {
public:
    explicit GateBlocked(std::string reason)
        : std::runtime_error(reason), reason_(std::move(reason)) {}

    [[nodiscard]] const std::string& reason() const noexcept {
        return reason_;
    }

private:
    std::string reason_;
};

std::string utf8(const std::wstring_view input) {
    if (input.empty()) return {};
    const auto required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return "<wide-string-conversion-failed>";
    std::string output(static_cast<std::size_t>(required), '\0');
    const auto converted = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
        static_cast<int>(input.size()), output.data(), required,
        nullptr, nullptr);
    return converted == required
        ? output
        : std::string("<wide-string-conversion-failed>");
}

std::string json_escape(const std::string_view input) {
    std::ostringstream output;
    for (const auto value : input) {
        switch (value) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (static_cast<unsigned char>(value) < 0x20U) {
                constexpr char digits[] = "0123456789abcdef";
                const auto byte = static_cast<unsigned char>(value);
                output << "\\u00" << digits[byte >> 4U]
                       << digits[byte & 0x0fU];
            } else {
                output << value;
            }
        }
    }
    return output.str();
}

void emit_report(const GateReport& report) {
    std::ostringstream output;
    output << "{\"schema\":\"myrd.m3.vhdx_compat.v1\""
           << ",\"status\":\"" << json_escape(report.status) << "\""
           << ",\"reason\":\"" << json_escape(report.reason) << "\""
           << ",\"elevated\":" << (report.elevated ? "true" : "false")
           << ",\"aim_devices_online\":" << report.aim_devices_online
           << ",\"cases\":[";
    for (std::size_t index = 0; index < report.cases.size(); ++index) {
        if (index != 0) output << ',';
        const auto& item = report.cases[index];
        output << "{\"name\":\"" << json_escape(item.name) << "\""
               << ",\"status\":\"" << json_escape(item.status) << "\""
               << ",\"operation\":\"" << json_escape(item.operation)
               << "\",\"detail\":\"" << json_escape(item.detail) << "\""
               << ",\"win32_code\":" << item.win32_code
               << ",\"cleanup_attempted\":"
               << (item.cleanup_attempted ? "true" : "false")
               << ",\"cleanup_succeeded\":"
               << (item.cleanup_succeeded ? "true" : "false")
               << ",\"cleanup_win32\":" << item.cleanup_win32
               << ",\"recovery_command\":\""
               << json_escape(item.recovery_command) << "\"}";
    }
    output << "]}";
    std::cout << output.str() << '\n';
}

[[noreturn]] void fail_case(
    CaseReport& report, std::string operation, const std::uint32_t code,
    std::string detail) {
    report.status = "FAIL";
    report.operation = operation;
    report.win32_code = code;
    report.detail = detail;
    throw GateFailure(std::move(operation), code, std::move(detail));
}

template <class T>
T require_product(myrd::Result<T> result, CaseReport& report,
                  const std::string_view fallback_operation) {
    if (!result.ok()) {
        const auto operation = result.error().operation.empty()
            ? std::string(fallback_operation)
            : utf8(result.error().operation);
        fail_case(report, operation, result.error().code,
                  utf8(result.error().message));
    }
    return result.move_value();
}

void require_product(
    const myrd::Result<void>& result, CaseReport& report,
    const std::string_view fallback_operation) {
    if (!result.ok()) {
        const auto operation = result.error().operation.empty()
            ? std::string(fallback_operation)
            : utf8(result.error().operation);
        fail_case(report, operation, result.error().code,
                  utf8(result.error().message));
    }
}

bool current_process_is_elevated() {
    UniqueHandle token;
    HANDLE raw_token{};
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) {
        throw GateFailure("OpenProcessToken", GetLastError(),
                          "could not inspect process elevation");
    }
    token.reset(raw_token);
    TOKEN_ELEVATION elevation{};
    DWORD returned{};
    if (!GetTokenInformation(
            token.get(), TokenElevation, &elevation, sizeof(elevation),
            &returned)) {
        throw GateFailure("GetTokenInformation(TokenElevation)",
                          GetLastError(),
                          "could not inspect process elevation");
    }
    return elevation.TokenIsElevated != 0;
}

class VirtdiskApi final {
public:
    using OpenFn = decltype(&OpenVirtualDisk);
    using AttachFn = decltype(&AttachVirtualDisk);
    using DetachFn = decltype(&DetachVirtualDisk);
    using PhysicalPathFn = decltype(&GetVirtualDiskPhysicalPath);
    using GetInfoFn = decltype(&GetVirtualDiskInformation);
    using CreateFn = decltype(&CreateVirtualDisk);

    VirtdiskApi() = default;
    ~VirtdiskApi() noexcept {
        if (module_ != nullptr) FreeLibrary(module_);
    }
    VirtdiskApi(const VirtdiskApi&) = delete;
    VirtdiskApi& operator=(const VirtdiskApi&) = delete;

    void load() {
        module_ = LoadLibraryW(L"virtdisk.dll");
        if (module_ == nullptr) {
            throw GateBlocked("virtdisk_library_unavailable:" +
                              std::to_string(GetLastError()));
        }
        open = load_one<OpenFn>("OpenVirtualDisk");
        attach = load_one<AttachFn>("AttachVirtualDisk");
        detach = load_one<DetachFn>("DetachVirtualDisk");
        physical_path = load_one<PhysicalPathFn>(
            "GetVirtualDiskPhysicalPath");
        get_info = load_one<GetInfoFn>("GetVirtualDiskInformation");
        create = load_one<CreateFn>("CreateVirtualDisk");
    }

    OpenFn open{};
    AttachFn attach{};
    DetachFn detach{};
    PhysicalPathFn physical_path{};
    GetInfoFn get_info{};
    CreateFn create{};

private:
    template <class Function>
    Function load_one(const char* name) {
        const auto address = GetProcAddress(module_, name);
        if (address == nullptr) {
            throw GateBlocked(std::string("virtdisk_api_unavailable:") +
                              name + ":" +
                              std::to_string(GetLastError()));
        }
        return reinterpret_cast<Function>(address);
    }

    HMODULE module_{};
};

std::int64_t require_no_online_aim_devices() {
    auto discovered = myrd::aim::discover_aim_adapter();
    if (!discovered.ok()) {
        const auto message = utf8(discovered.error().message);
        if (discovered.error().domain == myrd::ErrorDomain::protocol &&
            discovered.error().code == 0 &&
            message == "no AIM adapter found") {
            return 0;
        }
        throw GateBlocked(
            "aim_safety_inventory_unavailable:" +
            utf8(discovered.error().operation) + ":" +
            std::to_string(discovered.error().code) + ":" + message);
    }
    auto adapter_handle = discovered.move_value();
    myrd::aim::WindowsSrbTransport transport(adapter_handle.handle.get());
    myrd::aim::DirectAimAdapter adapter(transport);
    auto devices = adapter.list_devices();
    if (!devices.ok()) {
        throw GateBlocked(
            "aim_safety_inventory_unavailable:" +
            utf8(devices.error().operation) + ":" +
            std::to_string(devices.error().code));
    }
    const auto count = static_cast<std::int64_t>(devices.value().size());
    if (count != 0) {
        throw GateBlocked("aim_devices_online:" + std::to_string(count));
    }
    return count;
}

class Sandbox final {
public:
    Sandbox() {
        static std::uint64_t sequence{};
        root_ = std::filesystem::temp_directory_path() /
            (L"myrd-m3-vhdx-compat-" +
             std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(sequence++));
        std::error_code create_error;
        const auto created = std::filesystem::create_directory(
            root_, create_error);
        if (!created || create_error) {
            throw GateFailure(
                "create_compatibility_sandbox",
                create_error
                    ? static_cast<std::uint32_t>(create_error.value())
                    : static_cast<std::uint32_t>(ERROR_ALREADY_EXISTS),
                "refused to reuse an existing compatibility directory");
        }
        std::ofstream manifest(
            root_ / L"disk.rdm", std::ios::binary | std::ios::trunc);
        const std::vector<char> bytes(8192, 'm');
        manifest.write(bytes.data(),
                       static_cast<std::streamsize>(bytes.size()));
        manifest.close();
        if (!manifest) {
            throw GateFailure("create_test_manifest", ERROR_WRITE_FAULT,
                              "could not create compatibility sandbox");
        }
    }

    ~Sandbox() noexcept {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept {
        return root_;
    }

private:
    std::filesystem::path root_;
};

class MapPayloadSource final : public IVhdxPayloadSource {
public:
    std::map<std::uint64_t, std::vector<std::byte>> payloads;

    myrd::Result<void> read_payload(
        const OperationContext&, const std::uint64_t payload_index,
        const std::span<std::byte> destination) const override {
        const auto found = payloads.find(payload_index);
        if (found == payloads.end() ||
            found->second.size() != destination.size()) {
            return myrd::Result<void>::failure({
                myrd::ErrorDomain::internal, ERROR_INVALID_DATA,
                L"MapPayloadSource::read_payload",
                L"compatibility payload is missing"});
        }
        std::copy(found->second.begin(), found->second.end(),
                  destination.begin());
        return myrd::Result<void>::success();
    }
};

std::vector<std::byte> deterministic_payload(const std::uint64_t seed) {
    std::vector<std::byte> payload(kVhdxDataBlockBytes);
    auto state = seed;
    for (auto& value : payload) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        value = static_cast<std::byte>(state & 0xffU);
    }
    return payload;
}

VhdxWriterIdentity writer_identity(const std::uint8_t discriminator) {
    VhdxWriterIdentity identity{};
    identity.file_identity.file_write_guid[0] =
        static_cast<std::byte>(0x10U + discriminator);
    identity.file_identity.data_write_guid[0] =
        static_cast<std::byte>(0x40U + discriminator);
    identity.virtual_disk_id[0] =
        static_cast<std::byte>(0x70U + discriminator);
    return identity;
}

VhdxWriterLimits writer_limits() {
    return {4096, 4096, kVhdxDataBlockBytes};
}

struct ProductFixture final {
    std::string name;
    std::filesystem::path path;
    std::uint64_t virtual_size{};
    std::uint32_t logical_sector{};
    std::uint64_t sample_offset{};
    std::vector<std::byte> expected_sample;
};

ProductFixture create_product_fixture(
    BundleWriterLock& writer, const Sandbox& sandbox, CaseReport& report,
    const std::string& name, const std::uint8_t discriminator,
    const std::uint64_t virtual_size, const std::uint32_t logical_sector,
    const std::vector<std::uint64_t>& candidates,
    MapPayloadSource& source, const std::uint64_t sample_offset,
    std::vector<std::byte> expected_sample) {
    auto plan = require_product(
        plan_sparse_vhdx(
            OperationContext{}, {virtual_size, logical_sector},
            writer_identity(discriminator), candidates, source,
            writer_limits()),
        report, "plan_sparse_vhdx");
    const auto temporary = name + ".tmp.vhdx";
    const auto final_name = name + ".vhdx";
    auto published = require_product(
        publish_sparse_vhdx_base(
            OperationContext{}, writer, temporary, final_name, plan, source),
        report, "publish_sparse_vhdx_base");
    if (published.layout().virtual_size() != virtual_size ||
        published.layout().logical_sector_size() != logical_sector) {
        fail_case(report, "product_layout_check", ERROR_INVALID_DATA,
                  "product writer returned unexpected geometry");
    }
    return {name, sandbox.root() /
                      std::filesystem::path(published.relative_name()),
            virtual_size, logical_sector, sample_offset,
            std::move(expected_sample)};
}

std::wstring powershell_single_quote(const std::wstring_view value) {
    std::wstring result;
    result.reserve(value.size());
    for (const auto character : value) {
        result.push_back(character);
        if (character == L'\'') result.push_back(L'\'');
    }
    return result;
}

class AttachedDisk final {
public:
    AttachedDisk(
        const VirtdiskApi& api, const HANDLE disk, CaseReport& report,
        std::filesystem::path image_path)
        : api_(api), disk_(disk), report_(report),
          image_path_(std::move(image_path)) {}

    ~AttachedDisk() noexcept { static_cast<void>(detach_now()); }

    void mark_attached() noexcept { attached_ = true; }

    [[nodiscard]] bool detach_now() noexcept {
        if (!attached_) return report_.cleanup_succeeded;
        report_.cleanup_attempted = true;
        const auto code = api_.detach(
            disk_, DETACH_VIRTUAL_DISK_FLAG_NONE, 0);
        report_.cleanup_win32 = code;
        report_.cleanup_succeeded = code == ERROR_SUCCESS;
        attached_ = false;
        if (code != ERROR_SUCCESS) {
            report_.recovery_command = utf8(
                L"Dismount-DiskImage -ImagePath '" +
                powershell_single_quote(image_path_.wstring()) + L"'");
        }
        return code == ERROR_SUCCESS;
    }

private:
    const VirtdiskApi& api_;
    HANDLE disk_{};
    CaseReport& report_;
    std::filesystem::path image_path_;
    bool attached_{};
};

VIRTUAL_STORAGE_TYPE vhdx_storage_type() {
    return {VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,
            kMicrosoftVirtualStorageVendor};
}

UniqueHandle open_virtual_disk(
    const VirtdiskApi& api, const std::filesystem::path& path,
    const VIRTUAL_DISK_ACCESS_MASK access, CaseReport& report) {
    auto storage = vhdx_storage_type();
    OPEN_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = OPEN_VIRTUAL_DISK_VERSION_1;
    parameters.Version1.RWDepth = 1;
    HANDLE raw{};
    const auto code = api.open(
        &storage, path.c_str(), access, OPEN_VIRTUAL_DISK_FLAG_NONE,
        &parameters, &raw);
    if (code != ERROR_SUCCESS) {
        fail_case(report, "OpenVirtualDisk", code,
                  "Windows rejected the VHDX file");
    }
    return UniqueHandle(raw);
}

GET_VIRTUAL_DISK_INFO query_virtual_size(
    const VirtdiskApi& api, const HANDLE disk, CaseReport& report) {
    GET_VIRTUAL_DISK_INFO information{};
    information.Version = GET_VIRTUAL_DISK_INFO_SIZE;
    ULONG size = sizeof(information);
    const auto code = api.get_info(disk, &size, &information, nullptr);
    if (code != ERROR_SUCCESS) {
        fail_case(report, "GetVirtualDiskInformation", code,
                  "could not query VHDX geometry");
    }
    return information;
}

void attach_virtual_disk(
    const VirtdiskApi& api, const HANDLE disk,
    const ATTACH_VIRTUAL_DISK_FLAG flags, AttachedDisk& attached,
    CaseReport& report) {
    ATTACH_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = ATTACH_VIRTUAL_DISK_VERSION_1;
    const auto code = api.attach(
        disk, nullptr, flags, 0, &parameters, nullptr);
    if (code != ERROR_SUCCESS) {
        fail_case(report, "AttachVirtualDisk", code,
                  "Windows could not attach the VHDX");
    }
    attached.mark_attached();
}

std::wstring physical_path(
    const VirtdiskApi& api, const HANDLE disk, CaseReport& report) {
    std::vector<wchar_t> buffer(256);
    ULONG bytes = static_cast<ULONG>(buffer.size() * sizeof(wchar_t));
    auto code = api.physical_path(disk, &bytes, buffer.data());
    if (code == ERROR_INSUFFICIENT_BUFFER) {
        buffer.resize((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t));
        code = api.physical_path(disk, &bytes, buffer.data());
    }
    if (code != ERROR_SUCCESS) {
        fail_case(report, "GetVirtualDiskPhysicalPath", code,
                  "could not resolve attached PhysicalDrive");
    }
    return std::wstring(buffer.data());
}

UniqueHandle open_physical_disk(
    const std::wstring& path, const DWORD access, CaseReport& report) {
    for (std::uint32_t attempt = 0; attempt < 50; ++attempt) {
        UniqueHandle disk(CreateFileW(
            path.c_str(), access,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (disk) return disk;
        const auto code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_NOT_READY &&
            code != ERROR_SHARING_VIOLATION) {
            fail_case(report, "CreateFileW(PhysicalDrive)", code,
                      "could not open attached PhysicalDrive");
        }
        Sleep(100);
    }
    fail_case(report, "CreateFileW(PhysicalDrive)", GetLastError(),
              "attached PhysicalDrive did not become ready");
}

std::uint64_t physical_disk_size(
    const HANDLE disk, CaseReport& report) {
    std::array<std::byte, 1024> output{};
    DWORD returned{};
    if (!DeviceIoControl(
            disk, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0,
            output.data(), static_cast<DWORD>(output.size()), &returned,
            nullptr)) {
        fail_case(report, "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX",
                  GetLastError(), "could not query attached disk size");
    }
    if (returned < sizeof(DISK_GEOMETRY_EX)) {
        fail_case(report, "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX",
                  ERROR_INVALID_DATA, "geometry response was truncated");
    }
    const auto* geometry =
        reinterpret_cast<const DISK_GEOMETRY_EX*>(output.data());
    if (geometry->DiskSize.QuadPart < 0) {
        fail_case(report, "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX",
                  ERROR_INVALID_DATA, "geometry returned a negative size");
    }
    return static_cast<std::uint64_t>(geometry->DiskSize.QuadPart);
}

std::vector<std::byte> read_physical(
    const HANDLE disk, const std::uint64_t offset, const std::size_t length,
    CaseReport& report) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(disk, position, nullptr, FILE_BEGIN)) {
        fail_case(report, "SetFilePointerEx(PhysicalDrive)", GetLastError(),
                  "could not seek to sample offset");
    }
    std::vector<std::byte> output(length);
    DWORD read{};
    if (!ReadFile(
            disk, output.data(), static_cast<DWORD>(output.size()),
            &read, nullptr)) {
        fail_case(report, "ReadFile(PhysicalDrive)", GetLastError(),
                  "could not read sample bytes");
    }
    if (read != output.size()) {
        fail_case(report, "ReadFile(PhysicalDrive)", ERROR_HANDLE_EOF,
                  "physical sample read was short");
    }
    return output;
}

void write_physical(
    const HANDLE disk, const std::uint64_t offset,
    const std::span<const std::byte> bytes, CaseReport& report) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(disk, position, nullptr, FILE_BEGIN)) {
        fail_case(report, "SetFilePointerEx(PhysicalDrive write)",
                  GetLastError(), "could not seek native fixture");
    }
    DWORD written{};
    if (!WriteFile(
            disk, bytes.data(), static_cast<DWORD>(bytes.size()),
            &written, nullptr)) {
        fail_case(report, "WriteFile(PhysicalDrive)", GetLastError(),
                  "could not write native fixture data");
    }
    if (written != bytes.size()) {
        fail_case(report, "WriteFile(PhysicalDrive)", ERROR_WRITE_FAULT,
                  "native fixture write was short");
    }
    if (!FlushFileBuffers(disk)) {
        fail_case(report, "FlushFileBuffers(PhysicalDrive)", GetLastError(),
                  "could not flush native fixture data");
    }
}

void verify_product_fixture(
    const VirtdiskApi& api, const ProductFixture& fixture,
    CaseReport& report) {
    static_cast<void>(require_no_online_aim_devices());
    const auto access = static_cast<VIRTUAL_DISK_ACCESS_MASK>(
        VIRTUAL_DISK_ACCESS_ATTACH_RO | VIRTUAL_DISK_ACCESS_DETACH |
        VIRTUAL_DISK_ACCESS_GET_INFO);
    auto disk = open_virtual_disk(api, fixture.path, access, report);
    const auto information = query_virtual_size(api, disk.get(), report);
    if (information.Size.VirtualSize != fixture.virtual_size ||
        information.Size.BlockSize != kVhdxDataBlockBytes ||
        information.Size.SectorSize != fixture.logical_sector) {
        fail_case(report, "GetVirtualDiskInformation", ERROR_INVALID_DATA,
                  "Windows reported unexpected VHDX geometry");
    }

    AttachedDisk attached(api, disk.get(), report, fixture.path);
    const auto flags = static_cast<ATTACH_VIRTUAL_DISK_FLAG>(
        ATTACH_VIRTUAL_DISK_FLAG_READ_ONLY |
        ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER);
    attach_virtual_disk(api, disk.get(), flags, attached, report);
    const auto path = physical_path(api, disk.get(), report);
    auto physical = open_physical_disk(path, GENERIC_READ, report);
    if (physical_disk_size(physical.get(), report) != fixture.virtual_size) {
        fail_case(report, "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX",
                  ERROR_INVALID_DATA,
                  "attached disk size differs from product geometry");
    }
    const auto actual = read_physical(
        physical.get(), fixture.sample_offset,
        fixture.expected_sample.size(), report);
    if (actual != fixture.expected_sample) {
        fail_case(report, "ReadFile(PhysicalDrive)", ERROR_CRC,
                  "attached bytes differ from product payload");
    }
    physical.reset();
    if (!attached.detach_now()) {
        fail_case(report, "DetachVirtualDisk", report.cleanup_win32,
                  "finally-detach failed; run recovery_command");
    }
    report.status = "PASS";
    report.operation = "native_round_trip";
    report.detail = "Windows open/query/attach/sample/detach succeeded";
}

GUID native_fixture_guid() {
    return {0x79e01934, 0x6854, 0x4df6,
            {0x81, 0xf4, 0x99, 0x31, 0x2f, 0x7d, 0xb2, 0x41}};
}

void verify_windows_created_fixture(
    const VirtdiskApi& api, const Sandbox& sandbox, CaseReport& report) {
    static_cast<void>(require_no_online_aim_devices());
    const auto path = sandbox.root() / L"windows-native.vhdx";
    auto storage = vhdx_storage_type();
    CREATE_VIRTUAL_DISK_PARAMETERS parameters{};
    parameters.Version = CREATE_VIRTUAL_DISK_VERSION_2;
    parameters.Version2.UniqueId = native_fixture_guid();
    parameters.Version2.MaximumSize = 8 * kMiB;
    parameters.Version2.BlockSizeInBytes = kVhdxDataBlockBytes;
    parameters.Version2.SectorSizeInBytes = kLogicalSectorSize512;
    parameters.Version2.PhysicalSectorSizeInBytes = kPhysicalSectorSize;
    parameters.Version2.OpenFlags = OPEN_VIRTUAL_DISK_FLAG_NONE;
    HANDLE raw{};
    const auto create_code = api.create(
        &storage, path.c_str(), VIRTUAL_DISK_ACCESS_NONE, nullptr,
        CREATE_VIRTUAL_DISK_FLAG_NONE, 0, &parameters, nullptr, &raw);
    if (create_code != ERROR_SUCCESS) {
        fail_case(report, "CreateVirtualDisk", create_code,
                  "Windows could not create the independent VHDX fixture");
    }
    UniqueHandle disk(raw);
    const auto information = query_virtual_size(api, disk.get(), report);
    if (information.Size.VirtualSize != 8 * kMiB ||
        information.Size.BlockSize != kVhdxDataBlockBytes ||
        information.Size.SectorSize != kLogicalSectorSize512) {
        fail_case(report, "GetVirtualDiskInformation(native)",
                  ERROR_INVALID_DATA,
                  "Windows-created fixture has unexpected geometry");
    }

    AttachedDisk attached(api, disk.get(), report, path);
    attach_virtual_disk(
        api, disk.get(), ATTACH_VIRTUAL_DISK_FLAG_NO_DRIVE_LETTER,
        attached, report);
    const auto device_path = physical_path(api, disk.get(), report);
    auto physical = open_physical_disk(
        device_path, GENERIC_READ | GENERIC_WRITE, report);
    if (physical_disk_size(physical.get(), report) != 8 * kMiB) {
        fail_case(report, "IOCTL_DISK_GET_DRIVE_GEOMETRY_EX(native)",
                  ERROR_INVALID_DATA,
                  "refused to write a PhysicalDrive with unexpected size");
    }
    std::vector<std::byte> expected(64 * kKiB);
    auto state = std::uint64_t{0x510e527fade682d1ULL};
    for (auto& value : expected) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        value = static_cast<std::byte>(state & 0xffU);
    }
    write_physical(physical.get(), 0, expected, report);
    physical.reset();
    if (!attached.detach_now()) {
        fail_case(report, "DetachVirtualDisk(native)",
                  report.cleanup_win32,
                  "finally-detach failed; run recovery_command");
    }
    disk.reset();

    auto parser_capability = require_product(
        open_bundle_file_for_inspection(
            sandbox.root(), "windows-native.vhdx"),
        report, "open_bundle_file_for_inspection(native parser)");
    auto parser_file = require_product(
        open_validated_persistence_file(std::move(parser_capability)),
        report, "open_validated_persistence_file(native parser)");
    std::shared_ptr<IPersistenceFile> shared_parser_file(
        std::move(parser_file));
    auto layout = require_product(
        parse_vhdx_layout(OperationContext{}, shared_parser_file),
        report, "parse_vhdx_layout(native)");
    shared_parser_file.reset();
    if (layout.virtual_size() != 8 * kMiB ||
        layout.logical_sector_size() != kLogicalSectorSize512 ||
        layout.block_size() != kVhdxDataBlockBytes) {
        fail_case(report, "parse_vhdx_layout(native)", ERROR_INVALID_DATA,
                  "product parser returned unexpected native geometry");
    }

    auto reader_capability = require_product(
        open_bundle_file_for_inspection(
            sandbox.root(), "windows-native.vhdx"),
        report, "open_bundle_file_for_inspection(native reader)");
    auto reader_file = require_product(
        open_validated_persistence_file(std::move(reader_capability)),
        report, "open_validated_persistence_file(native reader)");
    auto reader = require_product(
        VhdxReader::create(
            OperationContext{}, std::move(reader_file), layout,
            {{16 * kVhdxBatPageBytes, 16}, 16, 16}),
        report, "VhdxReader::create(native)");
    std::vector<std::byte> actual(64 * kKiB, std::byte{0xa5});
    const auto loaded = require_product(
        reader->read_block(OperationContext{}, 0, actual),
        report, "VhdxReader::read_block(native)");
    if (loaded.kind != myrd::BlockLoadKind::Data || actual != expected) {
        fail_case(report, "VhdxReader::read_block(native)", ERROR_CRC,
                  "product reader differs from Windows-written data");
    }
    report.status = "PASS";
    report.operation = "windows_create_product_read";
    report.detail =
        "Windows-created VHDX geometry and payload matched product reader";
}

void run_gate(GateReport& report) {
    report.cases.reserve(5);
    report.elevated = current_process_is_elevated();
    if (!report.elevated) {
        throw GateBlocked("administrator_token_required");
    }

    VirtdiskApi api;
    api.load();
    report.aim_devices_online = require_no_online_aim_devices();

    Sandbox sandbox;
    CaseReport setup_case;
    setup_case.name = "product_fixture_setup";
    auto writer = require_product(
        acquire_bundle_writer_lock(sandbox.root(), "disk.rdm"),
        setup_case, "acquire_bundle_writer_lock");

    std::vector<ProductFixture> fixtures;
    fixtures.reserve(4);

    MapPayloadSource empty_source;
    fixtures.push_back(create_product_fixture(
        writer, sandbox, setup_case, "empty512", 1, 8 * kMiB,
        kLogicalSectorSize512, {}, empty_source, 0,
        std::vector<std::byte>(4096, std::byte{0})));

    MapPayloadSource sparse_source;
    sparse_source.payloads.emplace(1, deterministic_payload(0x101));
    const std::vector<std::uint64_t> sparse_candidates{1};
    fixtures.push_back(create_product_fixture(
        writer, sandbox, setup_case, "sparse512", 2, 8 * kMiB,
        kLogicalSectorSize512, sparse_candidates, sparse_source,
        kVhdxDataBlockBytes,
        std::vector<std::byte>(
            sparse_source.payloads.at(1).begin(),
            sparse_source.payloads.at(1).begin() + 4096)));

    constexpr std::uint64_t chunk_ratio_512 =
        (std::uint64_t{1} << 23U) * kLogicalSectorSize512 /
        kVhdxDataBlockBytes;
    MapPayloadSource boundary_source;
    boundary_source.payloads.emplace(
        chunk_ratio_512, deterministic_payload(0x202));
    const std::vector<std::uint64_t> boundary_candidates{chunk_ratio_512};
    fixtures.push_back(create_product_fixture(
        writer, sandbox, setup_case, "boundary512", 3,
        (chunk_ratio_512 + 1) * kVhdxDataBlockBytes,
        kLogicalSectorSize512, boundary_candidates, boundary_source,
        chunk_ratio_512 * kVhdxDataBlockBytes,
        std::vector<std::byte>(
            boundary_source.payloads.at(chunk_ratio_512).begin(),
            boundary_source.payloads.at(chunk_ratio_512).begin() + 4096)));

    MapPayloadSource sector4096_source;
    sector4096_source.payloads.emplace(0, deterministic_payload(0x303));
    const std::vector<std::uint64_t> sector4096_candidates{0};
    fixtures.push_back(create_product_fixture(
        writer, sandbox, setup_case, "sector4096", 4, 8 * kMiB,
        kLogicalSectorSize4096, sector4096_candidates,
        sector4096_source, 0,
        std::vector<std::byte>(
            sector4096_source.payloads.at(0).begin(),
            sector4096_source.payloads.at(0).begin() + 4096)));

    require_product(writer.close(), setup_case, "BundleWriterLock::close");

    for (const auto& fixture : fixtures) {
        report.cases.push_back({fixture.name});
        verify_product_fixture(api, fixture, report.cases.back());
    }

    report.cases.push_back({"windows_native_reader"});
    verify_windows_created_fixture(api, sandbox, report.cases.back());

    report.status = "PASS";
    report.reason = "all_native_vhdx_compatibility_cases_passed";
}

}  // namespace

int main() {
    GateReport report;
    try {
        run_gate(report);
    } catch (const GateBlocked& blocked) {
        report.status = "BLOCKED";
        report.reason = blocked.reason();
        emit_report(report);
        return kBlockedExitCode;
    } catch (const GateFailure& failure) {
        report.status = "FAIL";
        report.reason = failure.operation() + ":" +
            std::to_string(failure.code()) + ":" + failure.detail();
        emit_report(report);
        return 1;
    } catch (const std::exception& failure) {
        report.status = "FAIL";
        report.reason = std::string("unhandled_exception:") + failure.what();
        emit_report(report);
        return 1;
    } catch (...) {
        report.status = "FAIL";
        report.reason = "unhandled_nonstandard_exception";
        emit_report(report);
        return 1;
    }
    emit_report(report);
    return 0;
}
