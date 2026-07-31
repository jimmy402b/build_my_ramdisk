#pragma once

#include "result.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace myrd {

template <class To, class From>
Result<To> checked_numeric_cast(From value) noexcept {
    static_assert(std::is_integral_v<To>);
    static_assert(std::is_integral_v<From>);

    if constexpr (std::is_signed_v<From> && std::is_unsigned_v<To>) {
        if (value < 0) {
            return Result<To>::failure({ErrorDomain::internal, 0,
                L"checked_numeric_cast", L"negative value to unsigned type"});
        }
    }

    if (value > static_cast<From>(std::numeric_limits<To>::max())) {
        return Result<To>::failure({ErrorDomain::internal, 0,
            L"checked_numeric_cast", L"value exceeds target type maximum"});
    }

    if constexpr (std::is_unsigned_v<From> && std::is_signed_v<To>) {
        if (value > static_cast<From>(std::numeric_limits<To>::max())) {
            return Result<To>::failure({ErrorDomain::internal, 0,
                L"checked_numeric_cast", L"unsigned value exceeds signed target maximum"});
        }
    } else if constexpr (std::is_signed_v<From> && std::is_signed_v<To>) {
        if (value < static_cast<From>(std::numeric_limits<To>::min())) {
            return Result<To>::failure({ErrorDomain::internal, 0,
                L"checked_numeric_cast", L"value below target type minimum"});
        }
    }

    return Result<To>::success(static_cast<To>(value));
}

} // namespace myrd
