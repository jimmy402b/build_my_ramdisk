#include "tests/support/m4_fake_clock.h"

namespace myrd::tests {

FakeServiceClock::FakeServiceClock(
    std::uint64_t unix_ms,
    std::uint64_t monotonic_ms) noexcept
    : unix_ms_(unix_ms), monotonic_ms_(monotonic_ms) {}

std::uint64_t FakeServiceClock::unix_time_ms() const noexcept {
    return unix_ms_;
}

std::uint64_t FakeServiceClock::monotonic_time_ms() const noexcept {
    return monotonic_ms_;
}

void FakeServiceClock::set(
    std::uint64_t unix_ms,
    std::uint64_t monotonic_ms) noexcept {
    unix_ms_ = unix_ms;
    monotonic_ms_ = monotonic_ms;
}

void FakeServiceClock::advance(std::uint64_t milliseconds) noexcept {
    unix_ms_ += milliseconds;
    monotonic_ms_ += milliseconds;
}

} // namespace myrd::tests
