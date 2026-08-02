#include "test_framework.h"

#include "tests/support/fake_block_device.h"

TEST_CASE(m1_processor_fake_initial_counters_are_zero) {
    myrd::test::FakeBlockDevice fake;
    REQUIRE_EQ(fake.read_calls, 0u);
    REQUIRE_EQ(fake.write_calls, 0u);
    REQUIRE_EQ(fake.unmap_calls, 0u);
    REQUIRE_EQ(fake.zero_calls, 0u);
    REQUIRE_EQ(fake.total_calls(), 0u);
}
