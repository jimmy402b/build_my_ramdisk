#include "tests/support/test_framework.h"

#include "src/run/windows_volume.h"

#include <array>
#include <cstddef>
#include <string>
#include <type_traits>

namespace {
using namespace myrd;

ProductVolume expected_volume() {
    return {L"m4-owner", L"volume-a",
            {{7, 1ull << 20, 32ull << 20}}, L'R', false, true};
}

VolumeObservation exact_observation() {
    return {expected_volume(), false, false, true, false, false, true, true};
}

void require_error(const Error& error, VolumeError expected) {
    REQUIRE_EQ(error.code, static_cast<std::uint32_t>(expected));
}
} // namespace

TEST_CASE(m4_volume_canonical_guid_text_reads_win32_field_order) {
    // A Win32 GUID keeps Data1, Data2 and Data3 little-endian in memory while
    // Data4 is already in text order.  A plain byte-wise hex dump would produce
    // "44332211-6655-8877-..." -- a string that looks like a GUID, parses like a
    // GUID, and never matches the one Windows reports for the same volume.  This
    // is the whole reason the conversion is a named function with a test rather
    // than an inline loop at each call site.
    const std::array<std::byte, 16> guid{
        std::byte{0x44}, std::byte{0x33}, std::byte{0x22}, std::byte{0x11},
        std::byte{0x66}, std::byte{0x55},
        std::byte{0x88}, std::byte{0x77},
        std::byte{0x99}, std::byte{0xAA}, std::byte{0xBB}, std::byte{0xCC},
        std::byte{0xDD}, std::byte{0xEE}, std::byte{0xFF}, std::byte{0x00}};
    REQUIRE_EQ(canonical_guid_text(guid),
               std::string("11223344-5566-7788-99aa-bbccddeeff00"));
}

TEST_CASE(m4_volume_canonical_guid_text_is_lowercase_canonical_and_padded) {
    // Leading zeroes are significant and hex is lowercase: product::Uuid::parse
    // accepts only the 36-character canonical lowercase form, so a conversion
    // that dropped a zero or upper-cased a digit would produce an identity the
    // service cannot parse back.
    const std::array<std::byte, 16> guid{
        std::byte{0x00}, std::byte{0x00}, std::byte{0x00}, std::byte{0x01},
        std::byte{0x0A}, std::byte{0x00},
        std::byte{0x0B}, std::byte{0x00},
        std::byte{0x0C}, std::byte{0x0D}, std::byte{0x0E}, std::byte{0x0F},
        std::byte{0xA0}, std::byte{0xB0}, std::byte{0xC0}, std::byte{0xD0}};
    REQUIRE_EQ(canonical_guid_text(guid),
               std::string("01000000-000a-000b-0c0d-0e0fa0b0c0d0"));
}

TEST_CASE(m4_volume_persistent_identity_completes_only_with_all_three) {
    // The service records this triple into the durable profile and re-checks it
    // on every later start, so a partial identity is not a weaker proof -- it is
    // a proof that cannot be compared, and `complete()` is the single gate that
    // says so.  Two of three is incomplete, exactly like none of three.
    VolumePersistentIdentity identity{};
    REQUIRE(!identity.complete());
    identity.disk_guid = "11111111-2222-3333-4444-555555555555";
    REQUIRE(!identity.complete());
    identity.partition_guid = "66666666-7777-8888-9999-aaaaaaaaaaaa";
    REQUIRE(!identity.complete());
    identity.volume_guid = "bbbbbbbb-cccc-dddd-eeee-ffffffffffff";
    REQUIRE(identity.complete());
}

TEST_CASE(m4_volume_provision_requires_exact_extents_and_owner) {
    auto expected = expected_volume();
    auto observed = exact_observation();
    observed.volume.extents.front().length_bytes++;
    auto result = provision_new_volume(observed, expected);
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeError::ownership_mismatch);
}

TEST_CASE(m4_volume_recovery_refuses_foreign_mount_and_never_deletes_it) {
    auto foreign = exact_observation();
    foreign.foreign_mount = true;
    const std::array candidates{foreign};
    auto result = find_recovered_volume(candidates, expected_volume());
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeError::foreign_mount);
    // The seam has no delete operation: foreign resources are never owned.
}

TEST_CASE(m4_volume_assign_refuses_occupied_drive_letter) {
    auto observed = exact_observation();
    observed.drive_letter_occupied = true;
    auto result = assign_drive_letter(observed, expected_volume(), L'R');
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeError::occupied_drive_letter);
}

TEST_CASE(m4_volume_lock_reports_busy_as_success_and_timeout_as_failure) {
    auto observed = exact_observation();
    observed.lock_available = false;
    auto busy = lock_volume(observed, 0);
    REQUIRE(busy.ok());
    REQUIRE(busy.value().busy);
    REQUIRE(!busy.value().acquired);

    observed.lock_timed_out = true;
    auto timeout = lock_volume(observed, 1000);
    REQUIRE(!timeout.ok());
    require_error(timeout.error(), VolumeError::timeout);
}

TEST_CASE(m4_volume_lock_reports_non_busy_native_error_as_failure) {
    auto observed = exact_observation();
    observed.lock_api_error = true;
    auto result = lock_volume(observed, 1000);
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeError::platform_error);
}

TEST_CASE(m4_volume_lock_result_retains_move_only_handle_ownership) {
    static_assert(std::is_move_constructible_v<LockedVolume>);
    static_assert(!std::is_copy_constructible_v<LockedVolume>);
    static_assert(std::is_move_constructible_v<VolumeLockAttempt>);
    static_assert(!std::is_copy_constructible_v<VolumeLockAttempt>);

    // The policy seam models the native lock while the adapter owns the
    // native handle.  A successful policy result is acquired (not busy), and
    // callers must retain its move-only `locked` member until cleanup.
    auto observed = exact_observation();
    auto first = lock_volume(observed, 1000);
    REQUIRE(first.ok());
    REQUIRE(first.value().acquired);
    REQUIRE(!first.value().busy);
    REQUIRE(!first.value().locked.has_value());

    // A second attempt against an unavailable volume is reported as busy;
    // this is the retry-safe contract used by the shutdown path.
    observed.lock_available = false;
    auto second = lock_volume(observed, 1000);
    REQUIRE(second.ok());
    REQUIRE(!second.value().acquired);
    REQUIRE(second.value().busy);
}

TEST_CASE(m4_volume_flush_failure_prevents_checkpoint_barrier) {
    auto observed = exact_observation();
    observed.flush_succeeds = false;
    auto result = flush_volume(observed, expected_volume());
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeError::flush_failed);
}

TEST_CASE(m4_volume_cleanup_failure_is_reported) {
    auto observed = exact_observation();
    observed.cleanup_succeeds = false;
    auto result = remove_drive_letter(observed, expected_volume());
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeError::cleanup_failed);
}

TEST_CASE(m4_volume_recovery_ignores_unrelated_candidates) {
    auto foreign = exact_observation();
    foreign.volume.owner_token = L"other-owner";
    const std::array candidates{foreign, exact_observation()};
    auto result = find_recovered_volume(candidates, expected_volume());
    REQUIRE(result.ok());
    REQUIRE_EQ(result.value().owner_token, L"m4-owner");
}

TEST_CASE(m4_volume_recovery_accepts_exact_unmounted_candidate_for_remount) {
    auto candidate = exact_observation();
    candidate.volume.mounted = false;
    candidate.volume.drive_letter = L'R';
    const auto selected = select_volume_for_disk(
        std::array{candidate}, 7, L'R', L"runtime-owner", false);
    REQUIRE(selected.ok());
    REQUIRE(!selected.value().mounted);
    REQUIRE(selected.value().operation_owned);
    REQUIRE_EQ(selected.value().extents.front().disk_number, 7u);
}

TEST_CASE(m4_volume_remove_never_accepts_foreign_identity) {
    auto observed = exact_observation();
    observed.volume.owner_token = L"other-owner";
    auto result = remove_drive_letter(observed, expected_volume());
    REQUIRE(!result.ok());
    require_error(result.error(), VolumeError::ownership_mismatch);
}

TEST_CASE(m4_volume_bridge_requires_exactly_one_disk_candidate) {
    auto candidate = exact_observation();
    candidate.volume.mounted = true;
    auto selected = select_volume_for_disk(
        std::array{candidate}, 7, L'R', L"runtime-owner", true);
    REQUIRE(selected.ok());
    REQUIRE_EQ(selected.value().owner_token, L"runtime-owner");
    REQUIRE(selected.value().operation_owned);

    auto mismatch = candidate;
    mismatch.volume.extents.front().disk_number = 8;
    REQUIRE(!select_volume_for_disk(std::array{mismatch}, 7, L'R', L"o", true).ok());

    auto foreign_letter = candidate;
    foreign_letter.volume.drive_letter = L'S';
    REQUIRE(!select_volume_for_disk(std::array{foreign_letter}, 7, L'R', L"o", true).ok());
}

TEST_CASE(m4_volume_bridge_rejects_zero_and_multiple_candidates) {
    auto candidate = exact_observation();
    candidate.volume.mounted = true;
    REQUIRE(!select_volume_for_disk(std::span<const VolumeObservation>{},
                                    7, L'R', L"o", true).ok());
    auto second = candidate;
    second.volume.volume_guid = L"volume-b";
    auto ambiguous = select_volume_for_disk(std::array{candidate, second},
                                            7, L'R', L"o", true);
    REQUIRE(!ambiguous.ok());
    require_error(ambiguous.error(), VolumeError::ownership_mismatch);
}

TEST_CASE(m4_flush_refuses_identity_mismatch_before_barrier) {
    int flush_calls = 0;
    const auto result = flush_with_identity_checks(
        [] { return Result<void>::failure({ErrorDomain::win32,
            static_cast<std::uint32_t>(VolumeError::ownership_mismatch),
            L"verify_before", L"identity mismatch", 0}); },
        [&] { ++flush_calls; return Result<void>::success(); },
        [] { return Result<void>::success(); });
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().operation, std::wstring(L"verify_before"));
    REQUIRE_EQ(flush_calls, 0);
}

TEST_CASE(m4_flush_refuses_identity_mismatch_after_barrier) {
    int flush_calls = 0;
    const auto result = flush_with_identity_checks(
        [] { return Result<void>::success(); },
        [&] { ++flush_calls; return Result<void>::success(); },
        [] { return Result<void>::failure({ErrorDomain::win32,
            static_cast<std::uint32_t>(VolumeError::ownership_mismatch),
            L"verify_after", L"identity mismatch", 0}); });
    REQUIRE(!result.ok());
    REQUIRE_EQ(result.error().operation, std::wstring(L"verify_after"));
    REQUIRE_EQ(flush_calls, 1);
}

TEST_CASE(m4_flush_commits_only_between_two_successful_identity_checks) {
    std::string sequence;
    const auto result = flush_with_identity_checks(
        [&] { sequence += 'B'; return Result<void>::success(); },
        [&] { sequence += 'F'; return Result<void>::success(); },
        [&] { sequence += 'A'; return Result<void>::success(); });
    REQUIRE(result.ok());
    REQUIRE_EQ(sequence, std::string("BFA"));
}
