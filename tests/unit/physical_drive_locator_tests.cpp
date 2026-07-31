#include "test_framework.h"

#include "src/driver/physical_drive_locator.h"

#include <vector>

TEST_CASE(locator_matches_exact_path_target_lun) {
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x02, 0x03, 0x04, 5, 64ull*1024*1024, false});
    myrd::aim::DeviceId id{0x00040302u}; // path=2, target=3, lun=4
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.ok());
    REQUIRE_EQ(r.value().physical_drive_number, 5u);
}

TEST_CASE(locator_rejects_zero_matches) {
    std::vector<myrd::aim::DiskCandidate> cands;
    myrd::aim::DeviceId id{0x00000001u};
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.has_error());
}

TEST_CASE(locator_rejects_two_exact_matches) {
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x01, 0x00, 0x00, 5, 64ull*1024*1024, false});
    cands.push_back({0x01, 0x00, 0x00, 6, 64ull*1024*1024, false});
    myrd::aim::DeviceId id{0x00000001u};
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.has_error());
}

TEST_CASE(locator_rejects_candidate_that_existed_before) {
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x01, 0x00, 0x00, 5, 64ull*1024*1024, true});
    myrd::aim::DeviceId id{0x00000001u};
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.has_error());
}

TEST_CASE(locator_rejects_length_not_64_mib) {
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x01, 0x00, 0x00, 5, 1024, false});
    myrd::aim::DeviceId id{0x00000001u};
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.has_error());
}

TEST_CASE(locator_builds_exact_decimal_physicaldrive_path) {
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x01, 0x00, 0x00, 42, 64ull*1024*1024, false});
    myrd::aim::DeviceId id{0x00000001u};
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.ok());
    REQUIRE(r.value().path == L"\\\\.\\PhysicalDrive42");
}

TEST_CASE(locator_does_not_match_capacity_without_scsi_address) {
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x02, 0x00, 0x00, 5, 64ull*1024*1024, false});
    myrd::aim::DeviceId id{0x00000001u}; // different path
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.has_error());
}
