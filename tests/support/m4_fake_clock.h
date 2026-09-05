#pragma once

#include "src/management/service_core.h"

#include <cstdint>

namespace myrd::tests {

class FakeServiceClock final : public management::IServiceClock {
public:
    FakeServiceClock(
        std::uint64_t unix_ms,
        std::uint64_t monotonic_ms) noexcept;

    std::uint64_t unix_time_ms() const noexcept override;
    std::uint64_t monotonic_time_ms() const noexcept override;
    void set(std::uint64_t unix_ms, std::uint64_t monotonic_ms) noexcept;
    void advance(std::uint64_t milliseconds) noexcept;

private:
    std::uint64_t unix_ms_{};
    std::uint64_t monotonic_ms_{};
};

} // namespace myrd::tests
