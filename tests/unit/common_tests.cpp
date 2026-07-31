#include "test_framework.h"

TEST_CASE(repository_bootstrap_smoke) {
    REQUIRE(true);
    REQUIRE_EQ(sizeof(void*), 8u);
}
