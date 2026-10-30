#include "src/service/service_system.h"
#include "src/service/service_host.h"

#include <windows.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>

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
    // `ullTotalPageFile` is the commit limit -- the physical memory the OS can back with
    // RAM plus what it can back with paging files.  It is the number a fixed memory budget
    // must not exceed, because past it a commit is refused no matter how much RAM is free.
    // `ullTotalPhys` stays the separate reading it always was: the admission policy for
    // `automatic` is defined against physical memory and must not silently change.
    return Result<management::PhysicalMemorySnapshot>::success(
        {static_cast<std::uint64_t>(status.ullTotalPhys),
         static_cast<std::uint64_t>(status.ullAvailPhys),
         static_cast<std::uint64_t>(status.ullTotalPageFile)});
}

void StderrServiceLogger::write(
    const management::ServiceLogEvent& event) noexcept {
    // Best effort by contract -- the interface is `noexcept` -- because a
    // service must not fail an operation over its own inability to describe it.
    const auto kind = static_cast<unsigned>(event.kind);
    if (event.message.empty()) {
        std::fprintf(stderr, "myrd-service: kind=%u command=%s phase=%s\n", kind,
                     event.command.c_str(), event.phase.c_str());
    } else {
        // Printed when there is one, because the question an operator (and a test) has
        // about a failed operation is *why*, and a line that names only the phase answers
        // everything except that.
        std::fprintf(stderr, "myrd-service: kind=%u command=%s phase=%s message=%s\n",
                     kind, event.command.c_str(), event.phase.c_str(),
                     event.message.c_str());
    }
    std::fflush(stderr);

    // **And into a file, because stderr is discarded for an installed service.**
    //
    // The SCM starts the process with no console and its stderr goes nowhere anyone can
    // read, so every one of these lines was lost in exactly the configuration a user
    // runs.  Measured cost: after a hard power cut the service came back by itself and
    // the disk's start did not attach, and the only surviving statement was
    // `PersistenceBlocked` in `disk list` -- the reason had been written and thrown away.
    //
    // `%ProgramData%\MyRamDisk\logs\service.log`, appended, best effort.  A service that
    // cannot open its own log still serves: the failure is silent here and visible in the
    // event itself, which is the trade this interface already makes.
    static std::mutex log_mutex;
    static std::ofstream log_file;
    static bool log_opened = false;
    std::scoped_lock guard(log_mutex);
    if (!log_opened) {
        log_opened = true;
        wchar_t program_data[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableW(
            L"ProgramData", program_data, static_cast<DWORD>(std::size(program_data)));
        if (length != 0 && length < std::size(program_data)) {
            std::error_code ignored;
            const auto directory =
                std::filesystem::path(program_data) / L"MyRamDisk" / L"logs";
            std::filesystem::create_directories(directory, ignored);
            log_file.open(directory / L"service.log",
                          std::ios::out | std::ios::app);
        }
    }
    if (!log_file.is_open()) return;
    if (event.message.empty()) {
        log_file << "myrd-service: kind=" << kind << " command=" << event.command
                 << " phase=" << event.phase << '\n';
    } else {
        log_file << "myrd-service: kind=" << kind << " command=" << event.command
                 << " phase=" << event.phase << " message=" << event.message << '\n';
    }
    log_file.flush();
}

} // namespace myrd::service
