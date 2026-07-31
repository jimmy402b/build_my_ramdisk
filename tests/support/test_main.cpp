#include "test_framework.h"

#include <cstdio>

int main() {
    const auto& cases = myrd::test::registry();
    std::size_t passed = 0;
    std::size_t total = cases.size();

    for (const auto& tc : cases) {
        try {
            tc.func();
            std::printf("PASS %s\n", std::string(tc.name).c_str());
            ++passed;
        } catch (...) {
            std::printf("FAIL %s: exception\n", std::string(tc.name).c_str());
            return 1;
        }
    }

    std::printf("%zu/%zu passed\n", passed, total);
    return (passed == total) ? 0 : 1;
}
