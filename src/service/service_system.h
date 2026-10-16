#pragma once

// The service's production seams over Windows.
//
// These are the three interfaces `ServiceCore` needs that have no production
// implementation anywhere yet: they were reached only through test doubles,
// because until now nothing composed a real service.  Each is small on purpose
// and each is *only* what the core asks for -- a clock it can read, ids it can
// mint, and the machine's memory -- so that none of them becomes a second place
// where service behaviour lives.

#include "src/management/service_core.h"

#include <cstdint>

namespace myrd::service {

// The clock, over the machine's own.
//
// Wall time and monotonic time are deliberately read from *different* sources:
// `GetSystemTimePreciseAsFileTime` is the wall clock the durable records are
// stamped with, and `GetTickCount64` is a monotonic counter that cannot go
// backwards.  Deriving one from the other would make a durable timestamp jump
// when the operator fixes the clock, or when the machine resynchronizes, and the
// records are compared for ordering.
class SystemClock final : public management::IServiceClock {
public:
    [[nodiscard]] std::uint64_t unix_time_ms() const noexcept override;
    [[nodiscard]] std::uint64_t monotonic_time_ms() const noexcept override;
};

// Ids for admitted operations.
//
// Random rather than sequential: a service that restarted and re-minted ids from
// a counter would reissue an id a durable record already names, which is the one
// way two operations can become confusable in the store they are keyed by.
class SystemIdGenerator final : public management::IServiceIdGenerator {
public:
    [[nodiscard]] Result<product::Uuid> next() override;
};

// The machine's physical memory, for the admission policy.
class SystemPhysicalMemoryQuery final
    : public management::IPhysicalMemoryQuery {
public:
    [[nodiscard]] Result<management::PhysicalMemorySnapshot> query() override;
};

// Where the service's own record of what it did goes.
//
// A service has no console: it is started by the SCM with no window and its
// stdout goes nowhere anyone will look.  So this writes to the one channel that
// survives that, and it is used for the two things an operator needs from a
// service that will not start -- that it was reached, and why it stopped.
//
// It deliberately does not become a logging framework.  `ServiceLogEvent` is
// already the core's own vocabulary; anything more here would be a second
// schema for the same facts.
class StderrServiceLogger final : public management::IServiceLogger {
public:
    void write(const management::ServiceLogEvent& event) noexcept override;
};

} // namespace myrd::service
