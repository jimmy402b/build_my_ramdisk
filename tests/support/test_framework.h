#pragma once

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#pragma warning(disable: 4127) // conditional expression is constant in test macros

namespace myrd::test {

struct TestCase {
    std::string_view name;
    void (*func)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(std::string_view name, void (*func)()) {
        registry().push_back(TestCase{name, func});
    }
};

} // namespace myrd::test

#define TEST_CASE(name)                                                        \
    static void test_##name();                                                 \
    static ::myrd::test::Registrar reg_##name(#name, test_##name);             \
    static void test_##name()

#define REQUIRE(cond)                                                          \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << #cond << std::endl;                                   \
            std::_Exit(1);                                                      \
        }                                                                      \
    } while (0)
#define REQUIRE_EQ(a, b)                                                       \
    do {                                                                       \
        if (!((a) == (b))) {                                                   \
            std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << ": "        \
                      << #a << " == " << #b << std::endl;                      \
            std::_Exit(1);                                                      \
        }                                                                      \
    } while (0)

