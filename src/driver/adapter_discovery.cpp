#include "adapter_discovery.h"
#include "aim_protocol.h"
#include "srb_codec.h"
#include "windows_srb_transport.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

#include <memory>
#include <string>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

// GUID_DEVINTERFACE_STORAGEPORT from devguid.h
static const GUID kStoragePortGuid = {
    0x2ACCFE60, 0xC130, 0x11D1,
    {0xBA, 0x31, 0x00, 0xA0, 0x24, 0xAD, 0xAD, 0xE2}};

namespace myrd::aim {

namespace {

struct DevInfoDeleter {
    void operator()(HDEVINFO h) const {
        if (h && h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h);
    }
};
using DevInfoGuard = std::unique_ptr<
    std::remove_pointer_t<HDEVINFO>, DevInfoDeleter>;

bool service_matches_phdskmnt(HDEVINFO di, SP_DEVINFO_DATA& dd) {
    wchar_t buf[256]{};
    if (SetupDiGetDeviceRegistryPropertyW(di, &dd, SPDRP_SERVICE,
            nullptr, reinterpret_cast<PBYTE>(buf),
            static_cast<DWORD>(sizeof(buf)), nullptr)) {
        return _wcsicmp(buf, L"phdskmnt") == 0;
    }
    return false;
}

} // anonymous namespace

Result<AdapterHandle> discover_aim_adapter() {
    DevInfoGuard dev_info(
        SetupDiGetClassDevsW(&kStoragePortGuid, nullptr, nullptr,
            DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (!dev_info) {
        return Result<AdapterHandle>::failure(
            {ErrorDomain::win32, static_cast<std::uint32_t>(GetLastError()),
             L"SetupDiGetClassDevsW", L"failed to enumerate storage ports"});
    }

    std::vector<AdapterHandle> candidates;

    SP_DEVICE_INTERFACE_DATA iface_data{sizeof(SP_DEVICE_INTERFACE_DATA)};
    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(dev_info.get(), nullptr,
                                      &kStoragePortGuid, idx, &iface_data);
         ++idx) {

        // Get device info data for this interface
        SP_DEVINFO_DATA dev_info_data{sizeof(SP_DEVINFO_DATA)};

        // Get required size for detail
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(dev_info.get(), &iface_data,
            nullptr, 0, &required, nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) continue;

        std::vector<std::byte> detail_buf(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
            detail_buf.data());
        detail->cbSize = sizeof(*detail);

        if (!SetupDiGetDeviceInterfaceDetailW(dev_info.get(), &iface_data,
                detail, required, nullptr, &dev_info_data)) {
            continue;
        }

        // Check service is phdskmnt
        if (!service_matches_phdskmnt(dev_info.get(), dev_info_data)) {
            continue;
        }

        // Open device
        UniqueHandle h(CreateFileW(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!h) continue;

        // Probe QUERY_VERSION
        WindowsSrbTransport transport(h.get());
        auto resp = transport.transact(kQueryVersion, {}, 4, 30);
        if (!resp || resp.value().size() < 4) continue;

        candidates.push_back(
            {std::wstring(detail->DevicePath), std::move(h)});
    }

    if (candidates.empty()) {
        return Result<AdapterHandle>::failure(
            {ErrorDomain::protocol, 0,
             L"discover_aim_adapter", L"no AIM adapter found"});
    }
    if (candidates.size() > 1) {
        return Result<AdapterHandle>::failure(
            {ErrorDomain::protocol, 0,
             L"discover_aim_adapter",
             L"multiple AIM adapters found — ambiguous"});
    }

    return Result<AdapterHandle>::success(std::move(candidates[0]));
}

} // namespace myrd::aim
