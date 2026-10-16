#include "src/service/service_system.h"
#include "src/service/service_host.h"

#include <windows.h>

#include <cstdio>

namespace myrd::service {

std::uint64_t SystemClock::unix_time_ms() const noexcept {
    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);
    ULARGE_INTEGER value{};
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;
    // FILETIME counts 100-nanosecond intervals from 1601-01-01; Unix time counts
    // milliseconds from 1970-01-01.  The offset is the number of 100-ns ticks
    // between the two epochs, and the division is integer because the durable
    // records carry milliseconds.
    constexpr std::uint64_t kEpochDelta100ns = 116444736000000000ULL;
    if (value.QuadPart < kEpochDelta100ns) return 0;
    return (value.QuadPart - kEpochDelta100ns) / 10'000ULL;
}

std::uint64_t SystemClock::monotonic_time_ms() const noexcept {
    return static_cast<std::uint64_t>(GetTickCount64());
}

Result<product::Uuid> SystemIdGenerator::next() {
    return product::Uuid::random();
}

Result<management::PhysicalMemorySnapshot>
SystemPhysicalMemoryQuery::query() {
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (!GlobalMemoryStatusEx(&status)) {
        return Result<management::PhysicalMemorySnapshot>::failure(
            {ErrorDomain::win32,
             static_cast<std::uint32_t>(GetLastError()),
             L"GlobalMemoryStatusEx",
             L"physical memory could not be queried",
             GetLastError()});
    }
    return Result<management::PhysicalMemorySnapshot>::success(
        {static_cast<std::uint64_t>(status.ullTotalPhys),
         static_cast<std::uint64_t>(status.ullAvailPhys)});
}

void StderrServiceLogger::write(
    const management::ServiceLogEvent& event) noexcept {
    // Best effort by contract -- the interface is `noexcept` -- because a
    // service must not fail an operation over its own inability to describe it.
    const auto kind = static_cast<unsigned>(event.kind);
    std::fprintf(stderr, "myrd-service: kind=%u command=%s phase=%s\n", kind,
                 event.command.c_str(), event.phase.c_str());
    std::fflush(stderr);
}

} // namespace myrd::service
