#include "adapter_discovery.h"
#include "aim_protocol.h"
#include "srb_codec.h"
#include "windows_srb_transport.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <string>
#include <vector>

namespace myrd::aim {

Result<AdapterHandle> discover_aim_adapter() {
    std::vector<AdapterHandle> candidates;

    // Enumerate \\.\SCSI0: through \\.\SCSI9:
    for (int i = 0; i < 10; ++i) {
        wchar_t path[64];
        std::swprintf(path, 64, L"\\\\.\\SCSI%d:", i);

        UniqueHandle h(CreateFileW(path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (!h) {
            continue;
        }

        // Probe QUERY_VERSION
        WindowsSrbTransport transport(h.get());
        auto resp = transport.transact(kQueryVersion, {}, 4, 30);
        if (!resp || resp.value().size() < 4) {
            continue;
        }

        // Check that driver version is non-zero and plausible
        std::uint32_t ver = 0;
        std::memcpy(&ver, resp.value().data(), 4);
        if (ver == 0) {
            continue;
        }

        candidates.push_back({std::wstring(path), std::move(h)});
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
