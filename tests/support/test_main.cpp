#include "test_framework.h"

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

namespace {

// Diagnostics that survive a hard exit.
//
// A hard fault -- a stack-cookie fastfail, a corrupted heap list, or abort()
// reached through std::terminate -- kills the process WITHOUT flushing stdio.
// The "PASS <name>" lines are written to a fully buffered stdout, so a crashing
// run reports nothing at all: measured, a crashing run of
// unit.m2.backing_generation produced empty stdout and empty stderr, which made
// "which case was running" unanswerable.  (Transfer manifest section 1.31
// records the same lesson for the worker, and section 1.34 for naming a
// failing action.)
//
// So the case name goes to stderr -- which is unbuffered -- before the case
// runs, and the terminate path says why it fired.  The trace is opt-in through
// MYRD_TEST_TRACE so that the other 113 test binaries keep byte-identical
// stderr; nothing here changes what a case asserts or how it is judged.
bool tracing() noexcept {
    static const bool enabled = std::getenv("MYRD_TEST_TRACE") != nullptr;
    return enabled;
}

void report_terminate() noexcept {
    // Reached when an exception escapes a thread (that is outside main's
    // try/catch), or when the program calls std::terminate directly.  The
    // process is already dying, so an extra stderr line cannot change a passing
    // run; it only stops the reason from being lost.
    if (auto current = std::current_exception()) {
        try {
            std::rethrow_exception(current);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "MYRD_TERMINATE std::exception: %s\n", error.what());
        } catch (...) {
            std::fprintf(stderr, "MYRD_TERMINATE non-std exception\n");
        }
    } else {
        std::fprintf(stderr, "MYRD_TERMINATE no active exception\n");
    }
    std::fflush(stderr);
    // A distinctive code, so a terminate is never confused with the other
    // 0xC0000409 fastfail reasons.
    std::_Exit(70);
}

}  // namespace

int main() {
    std::set_terminate(&report_terminate);

    const auto& cases = myrd::test::registry();
    std::size_t passed = 0;
    std::size_t total = cases.size();

    for (const auto& tc : cases) {
        if (tracing()) {
            std::fprintf(stderr, "MYRD_RUN %s\n", std::string(tc.name).c_str());
            std::fflush(stderr);
        }
        try {
            tc.func();
            std::printf("PASS %s\n", std::string(tc.name).c_str());
            std::fflush(stdout);
            ++passed;
        } catch (...) {
            std::printf("FAIL %s: exception\n", std::string(tc.name).c_str());
            std::fflush(stdout);
            return 1;
        }
    }

    std::printf("%zu/%zu passed\n", passed, total);
    std::fflush(stdout);
    return (passed == total) ? 0 : 1;
}
