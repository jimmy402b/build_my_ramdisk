#include "test_framework.h"

#include "src/driver/physical_drive_locator.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>

TEST_CASE(locator_opens_disk_with_read_access_for_length_ioctl) {
    REQUIRE((myrd::aim::disk_candidate_open_access() & GENERIC_READ) != 0);
}

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

TEST_CASE(locator_ignores_existing_same_address_while_waiting_for_new_disk) {
    // VMware can report the same SCSI address for an existing disk and a
    // subsequently-created AIM disk. The old disk must not turn a
    // not-yet-enumerated new disk into a permanent identity conflict.
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x01, 0x00, 0x00, 5, 64ull * 1024ull * 1024ull, true});
    myrd::aim::DeviceId id{0x00000001u};
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.has_error());
    REQUIRE_EQ(r.error().code, static_cast<std::uint32_t>(ERROR_NOT_FOUND));
}

TEST_CASE(locator_rejects_length_not_64_mib) {
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x01, 0x00, 0x00, 5, 1024, false});
    myrd::aim::DeviceId id{0x00000001u};
    auto r = myrd::aim::match_created_disk(id, cands);
    REQUIRE(r.has_error());
}

TEST_CASE(locator_matches_an_explicit_64_gib_m2_capacity) {
    constexpr std::uint64_t m2_size = 64ull * 1024ull * 1024ull * 1024ull;
    std::vector<myrd::aim::DiskCandidate> cands;
    cands.push_back({0x01, 0x02, 0x03, 42, m2_size, false});
    myrd::aim::DeviceId id{0x00030201u};

    auto matched = myrd::aim::match_created_disk(id, cands, m2_size);
    REQUIRE(matched.ok());
    REQUIRE_EQ(matched.value().length_bytes, m2_size);

    auto wrong = myrd::aim::match_created_disk(
        id, cands, 32ull * 1024ull * 1024ull * 1024ull);
    REQUIRE(wrong.has_error());
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

TEST_CASE(locator_repeated_confirmation_requires_identical_identity) {
    const myrd::aim::LocatedDisk first{
        5, L"\\\\.\\PhysicalDrive5", 64ull * 1024ull * 1024ull,
        1, 2, 3};
    auto same = first;
    auto different = first;
    different.physical_drive_number = 6;

    REQUIRE(myrd::aim::same_located_disk_identity(first, same));
    REQUIRE(!myrd::aim::same_located_disk_identity(first, different));
}
