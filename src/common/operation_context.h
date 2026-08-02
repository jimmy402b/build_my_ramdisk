#pragma once

#include "result.h"

#include <chrono>
#include <stop_token>

namespace myrd {

struct OperationContext {
    std::stop_token stop_token{};
    std::chrono::steady_clock::time_point deadline =
        std::chrono::steady_clock::time_point::max();
};

inline Result<void> check_operation_context(
    const OperationContext& context) {
    if (context.stop_token.stop_requested()) {
        return Result<void>::failure({
            ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::cancelled),
            L"check_operation_context", L"operation cancelled"});
    }
    if (std::chrono::steady_clock::now() >= context.deadline) {
        return Result<void>::failure({
            ErrorDomain::storage,
            static_cast<std::uint32_t>(OperationErrorCode::timed_out),
            L"check_operation_context", L"operation deadline expired"});
    }
    return Result<void>::success();
}

} // namespace myrd
