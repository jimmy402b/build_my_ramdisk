#include "test_framework.h"

#include "src/common/operation_context.h"

#include <chrono>
#include <stop_token>

TEST_CASE(m1_operation_context_accepts_live_operation) {
    myrd::OperationContext context{
        {}, std::chrono::steady_clock::now() + std::chrono::seconds(1)};
    REQUIRE(myrd::check_operation_context(context).ok());
}

TEST_CASE(m1_operation_context_reports_cancellation_before_timeout) {
    std::stop_source source;
    source.request_stop();
    myrd::OperationContext context{
        source.get_token(), std::chrono::steady_clock::now() -
                                std::chrono::seconds(1)};
    auto result = myrd::check_operation_context(context);
    REQUIRE(result.has_error());
    REQUIRE_EQ(result.error().code,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::cancelled));
}

TEST_CASE(m1_operation_context_reports_expired_deadline) {
    myrd::OperationContext context{
        {}, std::chrono::steady_clock::now() - std::chrono::milliseconds(1)};
    auto result = myrd::check_operation_context(context);
    REQUIRE(result.has_error());
    REQUIRE_EQ(result.error().code,
        static_cast<std::uint32_t>(myrd::OperationErrorCode::timed_out));
}
