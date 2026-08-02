#include "test_framework.h"

#include "src/proxy/proxy_error.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

struct MappingCase {
    myrd::OperationErrorCode code;
    std::uint32_t native_code;
    std::uint64_t expected;
};

const MappingCase kMappingCases[] = {
    {myrd::OperationErrorCode::invalid_range, 0, ERROR_INVALID_PARAMETER},
    {myrd::OperationErrorCode::misaligned, 0, ERROR_INVALID_PARAMETER},
    {myrd::OperationErrorCode::read_only, 0, ERROR_WRITE_PROTECT},
    {myrd::OperationErrorCode::backing_read_failed, 0, ERROR_READ_FAULT},
    {myrd::OperationErrorCode::memory_hard_limit, 0, ERROR_NOT_ENOUGH_MEMORY},
    {myrd::OperationErrorCode::commit_failed, ERROR_COMMITMENT_LIMIT,
        ERROR_COMMITMENT_LIMIT},
    {myrd::OperationErrorCode::decommit_failed, 0, ERROR_NOT_ENOUGH_MEMORY},
    {myrd::OperationErrorCode::cancelled, 0, ERROR_OPERATION_ABORTED},
    {myrd::OperationErrorCode::timed_out, 0, ERROR_TIMEOUT},
    {myrd::OperationErrorCode::session_closed, 0, ERROR_BROKEN_PIPE},
    {myrd::OperationErrorCode::internal_invariant, 0, ERROR_INTERNAL_ERROR}
};

} // anonymous namespace

TEST_CASE(m1_proxy_error_maps_all_cases) {
    for (const auto& c : kMappingCases) {
        myrd::Error error{myrd::ErrorDomain::storage,
            static_cast<std::uint32_t>(c.code),
            L"test", L"test", c.native_code};
        auto errorno = myrd::proxy::to_proxy_errorno(error);
        REQUIRE_EQ(errorno, c.expected);
    }
}

TEST_CASE(m1_proxy_error_native_code_priority) {
    myrd::Error error{myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::commit_failed),
        L"test", L"test", ERROR_COMMITMENT_LIMIT};
    auto errorno = myrd::proxy::to_proxy_errorno(error);
    REQUIRE_EQ(errorno, static_cast<std::uint64_t>(ERROR_COMMITMENT_LIMIT));
}

TEST_CASE(m1_proxy_error_unknown_code_maps_internal_error) {
    myrd::Error error{myrd::ErrorDomain::storage, 0xFFFF,
        L"test", L"test", 0};
    auto errorno = myrd::proxy::to_proxy_errorno(error);
    REQUIRE_EQ(errorno, static_cast<std::uint64_t>(ERROR_INTERNAL_ERROR));
}

TEST_CASE(m1_proxy_error_request_level_codes) {
    myrd::Error e1{myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::invalid_range),
        L"t", L"t"};
    REQUIRE(myrd::proxy::classify_error(e1)
        == myrd::proxy::ErrorDisposition::request);
    myrd::Error e2{myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::cancelled),
        L"t", L"t"};
    REQUIRE(myrd::proxy::classify_error(e2)
        == myrd::proxy::ErrorDisposition::request);
    myrd::Error e3{myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::timed_out),
        L"t", L"t"};
    REQUIRE(myrd::proxy::classify_error(e3)
        == myrd::proxy::ErrorDisposition::request);
}

TEST_CASE(m1_proxy_error_fatal_codes) {
    myrd::Error e1{myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::protocol_malformed),
        L"t", L"t"};
    REQUIRE(myrd::proxy::classify_error(e1)
        == myrd::proxy::ErrorDisposition::session_fatal);
    myrd::Error e2{myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::unknown_request),
        L"t", L"t"};
    REQUIRE(myrd::proxy::classify_error(e2)
        == myrd::proxy::ErrorDisposition::session_fatal);
    myrd::Error e3{myrd::ErrorDomain::storage,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::internal_invariant),
        L"t", L"t"};
    REQUIRE(myrd::proxy::classify_error(e3)
        == myrd::proxy::ErrorDisposition::session_fatal);
}

TEST_CASE(m1_proxy_error_unknown_code_is_fatal) {
    myrd::Error e{myrd::ErrorDomain::storage, 0xFFFF, L"t", L"t"};
    REQUIRE(myrd::proxy::classify_error(e)
        == myrd::proxy::ErrorDisposition::session_fatal);
}
