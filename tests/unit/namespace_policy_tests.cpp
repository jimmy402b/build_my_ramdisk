#include "test_framework.h"

#include "src/proxy/object_names.h"

#include <algorithm>

using namespace myrd;
using namespace myrd::proxy;

static constexpr std::wstring_view kValidId = L"00112233445566778899aabbccddeeff";

TEST_CASE(m1_namespace_kernel_policy_builds_all_six_fields) {
    KernelVisibleNamespacePolicy policy;
    auto r = policy.build_names(kValidId);
    REQUIRE(r.ok());
    auto& n = r.value();
    REQUIRE(n.mapping  == L"Global\\myrd-m1-00112233445566778899aabbccddeeff");
    REQUIRE(n.request  == L"Global\\myrd-m1-00112233445566778899aabbccddeeff_Request");
    REQUIRE(n.response == L"Global\\myrd-m1-00112233445566778899aabbccddeeff_Response");
    REQUIRE(n.cancel   == L"Global\\myrd-m1-00112233445566778899aabbccddeeff_Cancel");
    REQUIRE(n.server   == L"Global\\myrd-m1-00112233445566778899aabbccddeeff_Server");
    REQUIRE(n.driver_path == L"\\BaseNamedObjects\\Global\\myrd-m1-00112233445566778899aabbccddeeff");
}

TEST_CASE(m1_namespace_local_policy_uses_local_prefix) {
    LocalNamespacePolicy policy;
    auto r = policy.build_names(kValidId);
    REQUIRE(r.ok());
    auto& n = r.value();
    REQUIRE(n.mapping == L"Local\\myrd-m1-test-00112233445566778899aabbccddeeff");
    REQUIRE(n.request == L"Local\\myrd-m1-test-00112233445566778899aabbccddeeff_Request");
}

TEST_CASE(m1_namespace_local_policy_has_empty_driver_path) {
    LocalNamespacePolicy policy;
    auto r = policy.build_names(kValidId);
    REQUIRE(r.ok());
    REQUIRE(r.value().driver_path.empty());
}

TEST_CASE(m1_namespace_policies_reject_invalid_id) {
    KernelVisibleNamespacePolicy kp;
    LocalNamespacePolicy lp;
    REQUIRE(kp.build_names(L"bad").has_error());
    REQUIRE(lp.build_names(L"BAD1234567890123456789012345678").has_error());
}

TEST_CASE(m1_namespace_policies_require_32_chars) {
    KernelVisibleNamespacePolicy policy;
    REQUIRE(policy.build_names(L"abc123").has_error());
    REQUIRE(policy.build_names(L"00112233445566778899aabbccddeeff00").has_error());
}

TEST_CASE(m1_object_names_from_session_still_produces_m0_names) {
    auto r = ObjectNames::from_session(kValidId);
    REQUIRE(r.ok());
    auto& n = r.value();
    REQUIRE(n.mapping == L"Global\\myrd-m0-00112233445566778899aabbccddeeff");
    REQUIRE(n.cancel  == L"Global\\myrd-m0-00112233445566778899aabbccddeeff_Cancel");
}
