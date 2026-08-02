#include "test_framework.h"

#include "src/proxy/object_names.h"

#include <algorithm>
#include <set>

TEST_CASE(object_names_accept_32_lowercase_hex_session) {
    auto r = myrd::proxy::ObjectNames::from_session(
        L"00112233445566778899aabbccddeeff");
    REQUIRE(r.ok());
}

TEST_CASE(object_names_build_exact_win32_names) {
    auto r = myrd::proxy::ObjectNames::from_session(
        L"00112233445566778899aabbccddeeff");
    REQUIRE(r.ok());
    auto& n = r.value();
    REQUIRE(n.mapping  == L"Global\\myrd-m0-00112233445566778899aabbccddeeff");
    REQUIRE(n.request  == L"Global\\myrd-m0-00112233445566778899aabbccddeeff_Request");
    REQUIRE(n.response == L"Global\\myrd-m0-00112233445566778899aabbccddeeff_Response");
    REQUIRE(n.server   == L"Global\\myrd-m0-00112233445566778899aabbccddeeff_Server");
    REQUIRE(n.cancel   == L"Global\\myrd-m0-00112233445566778899aabbccddeeff_Cancel");
}

TEST_CASE(object_names_build_exact_native_path) {
    auto r = myrd::proxy::ObjectNames::from_session(
        L"00112233445566778899aabbccddeeff");
    REQUIRE(r.ok());
    REQUIRE(r.value().driver_path ==
        L"\\BaseNamedObjects\\Global\\myrd-m0-00112233445566778899aabbccddeeff");
}

TEST_CASE(object_names_reject_uppercase) {
    auto r = myrd::proxy::ObjectNames::from_session(
        L"00112233445566778899AABBCCDDEEFF");
    REQUIRE(r.has_error());
}

TEST_CASE(object_names_reject_wrong_length) {
    auto r = myrd::proxy::ObjectNames::from_session(L"abc");
    REQUIRE(r.has_error());
}

TEST_CASE(object_names_reject_separator) {
    auto r = myrd::proxy::ObjectNames::from_session(
        L"00112233-445566778899aabbccddeeff");
    REQUIRE(r.has_error());
}

TEST_CASE(object_names_reject_non_hex) {
    auto r = myrd::proxy::ObjectNames::from_session(
        L"00112233445566778899aabbccddeegz");
    REQUIRE(r.has_error());
}

TEST_CASE(session_generator_produces_128_bits_and_unique_values) {
    auto r1 = myrd::proxy::generate_session_id();
    REQUIRE(r1.ok());
    REQUIRE(r1.value().size() == 32u);
    auto r2 = myrd::proxy::generate_session_id();
    REQUIRE(r2.ok());
    // Extremely unlikely to collide
    REQUIRE(r1.value() != r2.value());
    // All lowercase hex
    for (auto ch : r1.value()) {
        REQUIRE((ch >= L'0' && ch <= L'9') || (ch >= L'a' && ch <= L'f'));
    }
}
