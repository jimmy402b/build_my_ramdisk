#include "adapter_discovery.h"
#include "aim_protocol.h"
#include "srb_codec.h"
#include "windows_srb_transport.h"

#define NOMINMAX
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>

// GUID_DEVICEINTERFACE_STORAGEPORT from Windows SDK winioctl.h:
// {0x2ACCFE60, 0xC130, 0x11D2, {0xB0,0x82,0x00,0xA0,0xC9,0x1E,0xFB,0x8B}}
static const GUID kStoragePortGuid = {
    0x2ACCFE60, 0xC130, 0x11D2,
    {0xB0, 0x82, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B}};

#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

namespace myrd::aim {

namespace {

struct DevInfoDeleter {
    void operator()(HDEVINFO h) const {
        if (h && h != INVALID_HANDLE_VALUE) SetupDiDestroyDeviceInfoList(h);
    }
};
using DevInfoGuard = std::unique_ptr<
    std::remove_pointer_t<HDEVINFO>, DevInfoDeleter>;

bool service_matches(std::wstring_view name) {
    // Case-insensitive match for phdskmnt
    if (name.size() < 8) return false;
    const wchar_t* target = L"phdskmnt";
    for (int i = 0; i < 8; ++i) {
        if (towlower(name[i]) != target[i]) return false;
    }
    return name.size() == 8 || name[8] == L'\0';
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
    std::vector<std::wstring> errors;  // collect per-candidate errors

    SP_DEVICE_INTERFACE_DATA iface_data{sizeof(SP_DEVICE_INTERFACE_DATA)};
    for (DWORD idx = 0;
         SetupDiEnumDeviceInterfaces(dev_info.get(), nullptr, &kStoragePortGuid,
                                      idx, &iface_data);
         ++idx) {

        // Get required size for detail
        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(dev_info.get(), &iface_data,
            nullptr, 0, &required, nullptr);
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
            errors.push_back(L"SetupDiGetDeviceInterfaceDetailW size query failed, err=" +
                             std::to_wstring(GetLastError()));
            continue;
        }

        std::vector<std::byte> detail_buf(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(
            detail_buf.data());
        detail->cbSize = sizeof(*detail);

        SP_DEVINFO_DATA dev_info_data{sizeof(SP_DEVINFO_DATA)};

        if (!SetupDiGetDeviceInterfaceDetailW(dev_info.get(), &iface_data,
                detail, required, nullptr, &dev_info_data)) {
            errors.push_back(L"SetupDiGetDeviceInterfaceDetailW failed, err=" +
                             std::to_wstring(GetLastError()));
            continue;
        }

        // Get service name via SPDRP_SERVICE
        wchar_t svc_buf[256]{};
        if (!SetupDiGetDeviceRegistryPropertyW(dev_info.get(), &dev_info_data,
                SPDRP_SERVICE, nullptr,
                reinterpret_cast<PBYTE>(svc_buf),
                static_cast<DWORD>(sizeof(svc_buf)), nullptr)) {
            errors.push_back(L"no service property for " +
                             std::wstring(detail->DevicePath));
            continue;
        }
        if (!service_matches(svc_buf)) {
            continue;  // not phdskmnt — silently skip
        }

        // Open device
        UniqueHandle h(CreateFileW(detail->DevicePath,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!h) {
            errors.push_back(L"CreateFileW failed for " +
                             std::wstring(detail->DevicePath) +
                             L", err=" + std::to_wstring(GetLastError()));
            continue;
        }

        // Probe QUERY_VERSION
        WindowsSrbTransport transport(h.get());
        auto resp = transport.transact(kQueryVersion, {}, 4, 30);
        if (!resp) {
            errors.push_back(L"QUERY_VERSION probe failed for " +
                             std::wstring(detail->DevicePath) +
                             L": " + resp.error().message);
            continue;
        }
        // API version must match kDriverVersion
        if (resp.value().return_code != kDriverVersion) {
            errors.push_back(L"API version mismatch for " +
                             std::wstring(detail->DevicePath) +
                             L": expected 0x" +
                             std::to_wstring(kDriverVersion) +
                             L", got 0x" +
                             std::to_wstring(resp.value().return_code));
            continue;
        }

        candidates.push_back(
            {std::wstring(detail->DevicePath), std::move(h)});
    }

    if (candidates.empty()) {
        // Build detailed error
        std::wstring detail = L"no AIM adapter found";
        if (!errors.empty()) {
            detail += L": ";
            for (size_t i = 0; i < errors.size() && i < 3; ++i) {
                if (i > 0) detail += L"; ";
                detail += errors[i];
            }
            if (errors.size() > 3) {
                detail += L" (+" + std::to_wstring(errors.size() - 3) +
                          L" more)";
            }
        }
        return Result<AdapterHandle>::failure(
            {ErrorDomain::protocol, 0,
             L"discover_aim_adapter", detail});
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
