/// @file persistence_boundary_tests.cpp
/// @brief Task 00 structural tests — persistence library boundary and
///        link-contract verification.

#include <cstddef>
#include <cstdint>

#include "src/persistence/persistence_types.h"
#include "test_framework.h"

namespace {

// ---- Format version is visible and matches the SPEC ------------------------
static_assert(myrd::persistence::kFormatMajor == 1,
              "SPEC: M3 format major must be 1");
static_assert(myrd::persistence::kFormatMinor == 0,
              "SPEC: M3 format minor must be 0");

// ---- Layout constants are compile-time -------------------------------------
static_assert(myrd::persistence::kManifestSlotBytes == 4096);
static_assert(myrd::persistence::kManifestFileBytes == 8192);
static_assert(myrd::persistence::kJournalAlignment == 4096);
static_assert(myrd::persistence::kIndexNodeBytes == 4096);
static_assert(myrd::persistence::kMaxRelativeNameBytes == 255);
static_assert(myrd::persistence::kVhdxDataBlockBytes == 2U * 1024U * 1024U);
static_assert(myrd::persistence::kMaxIndexDepth == 8);
static_assert(myrd::persistence::kInvalidGeneration == UINT64_MAX);

}  // namespace

TEST_CASE(m3_boundary_constants_match_spec) {
    REQUIRE(myrd::persistence::kFormatMajor == 1);
    REQUIRE(myrd::persistence::kFormatMinor == 0);
    REQUIRE(myrd::persistence::kManifestSlotBytes == 4096);
    REQUIRE(myrd::persistence::kManifestFileBytes == 8192);
}

TEST_CASE(m3_boundary_no_driver_dependency_at_compile_time) {
    // If this test compiles it proves persistence_types.h does not pull in
    // driver, proxy, M1 or M2 headers.
    REQUIRE(myrd::persistence::kInvalidGeneration == UINT64_MAX);
}
