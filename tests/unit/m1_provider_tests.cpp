#include "test_framework.h"

#include "src/m1/m1_provider.h"
#include "src/proxy/object_names.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <chrono>

namespace {

bool names_are_absent(const myrd::proxy::ProxyObjectNames& names) {
    HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, names.server.c_str());
    if (handle != nullptr) {
        CloseHandle(handle);
        return false;
    }
    handle = OpenFileMappingW(FILE_MAP_READ, FALSE, names.mapping.c_str());
    if (handle != nullptr) {
        CloseHandle(handle);
        return false;
    }
    for (const auto* name : {
             names.request.c_str(), names.response.c_str(), names.cancel.c_str()}) {
        handle = OpenEventW(SYNCHRONIZE, FALSE, name);
        if (handle != nullptr) {
            CloseHandle(handle);
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE(m1_provider_composes_local_test_session_and_releases_names) {
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = myrd::M1Provider::create_for_test(
        policy, {8ull * 1024ull * 1024ull, 512, std::chrono::seconds(2)});
    REQUIRE(provider.ok());
    const auto names = provider.value()->names();
    REQUIRE(provider.value()->native_object_path().empty());
    REQUIRE(provider.value()->start().ok());
    REQUIRE(provider.value()->snapshot().state ==
            myrd::proxy::SessionState::serving);
    REQUIRE(provider.value()->stop().ok());
    REQUIRE(names_are_absent(names));
}

TEST_CASE(m1_provider_uses_unique_names_and_rejects_second_start) {
    myrd::proxy::LocalNamespacePolicy policy;
    auto first = myrd::M1Provider::create_for_test(policy);
    auto second = myrd::M1Provider::create_for_test(policy);
    REQUIRE(first.ok());
    REQUIRE(second.ok());
    REQUIRE(first.value()->names().mapping != second.value()->names().mapping);
    REQUIRE(first.value()->start().ok());
    REQUIRE(first.value()->start().has_error());
    REQUIRE(first.value()->stop().ok());
    REQUIRE(second.value()->stop().ok());
}

TEST_CASE(m1_provider_stop_before_start_and_repeated_stop_are_safe) {
    myrd::proxy::LocalNamespacePolicy policy;
    auto provider = myrd::M1Provider::create_for_test(policy);
    REQUIRE(provider.ok());
    const auto names = provider.value()->names();
    REQUIRE(provider.value()->stop().ok());
    REQUIRE(provider.value()->stop().ok());
    REQUIRE(names_are_absent(names));
}
