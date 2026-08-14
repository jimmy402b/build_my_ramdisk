#include "src/driver/adapter_discovery.h"
#include "src/driver/direct_aim_adapter.h"
#include "src/driver/windows_srb_transport.h"

#include <cstddef>
#include <iostream>

int wmain() {
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

    std::wcout << L"{\"success\":true,\"api_version\":" << version.value().api_version
               << L",\"sub_version\":" << version.value().sub_version << L",\"ids\":[";
    for (std::size_t index = 0; index < devices.value().size(); ++index) {
        if (index != 0) std::wcout << L',';
        std::wcout << devices.value()[index].value;
    }
    std::wcout << L"]}" << std::endl;
    return 0;
}
