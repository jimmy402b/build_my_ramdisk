#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/windows_srb_transport.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <ostream>
#include <vector>

// Frozen success protocol: exit 0, empty stderr, and exactly one JSON object
// on stdout with a boolean `success`, numeric `api_version` / `sub_version`
// and a numeric `ids` array.  The qualification controller parses these bytes
// strictly, so the emission lives in one auditable place that a unit test can
// exercise without a driver.
inline void write_m4_query_json(std::wostream& out,
                                std::uint32_t api_version,
                                std::uint32_t sub_version,
                                const std::vector<std::uint32_t>& ids) {
    out << L"{\"success\":true,\"api_version\":" << api_version
        << L",\"sub_version\":" << sub_version << L",\"ids\":[";
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index != 0) out << L',';
        out << ids[index];
    }
    out << L"]}" << std::endl;
}

#ifndef MYRD_M4_AIM_QUERY_NO_MAIN
int wmain() {
    // Frozen driver/AIM discovery failure protocol: exit 1, empty stdout,
    // stderr exactly "discover_aim_adapter failed" + newline.
    auto discovered = myrd::aim::discover_aim_adapter();
    if (!discovered) {
        std::wcerr << L"discover_aim_adapter failed" << std::endl;
        return 1;
    }

    myrd::aim::WindowsSrbTransport transport(discovered.value().handle.get());
    myrd::aim::DirectAimAdapter adapter(transport);
    auto version = adapter.query_version();
    if (!version) {
        std::wcerr << L"query_version failed" << std::endl;
        return 2;
    }
    auto devices = adapter.list_devices();
    if (!devices) {
        std::wcerr << L"list_devices failed" << std::endl;
        return 3;
    }

    std::vector<std::uint32_t> ids;
    ids.reserve(devices.value().size());
    for (const auto& device : devices.value()) {
        ids.push_back(static_cast<std::uint32_t>(device.value));
    }
    write_m4_query_json(std::wcout, version.value().api_version,
                        version.value().sub_version, ids);
    return 0;
}
#endif
