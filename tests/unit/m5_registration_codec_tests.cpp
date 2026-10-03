// The durable profile-state record's codec.
//
// It had no direct coverage before this file -- nothing in the suite included
// the codec header -- and the widening that gave
// `CheckpointCommitBoundaryEvidence` a manifest slot and a bytes digest made it
// load-bearing.  A durable format change that nothing tests is a change whose
// failure mode is discovered on a user's disk.
//
// The two properties that matter are that the new fields survive a round trip,
// and that a record written by the *previous* schema minor still decodes -- with
// the two fields absent rather than invented, because a digest of zeroes that
// compared equal would turn an unprovable operation into a proved one.

#include "tests/support/test_framework.h"

#include "src/common/sha256.h"
#include "src/management/profile_registration_codec.h"
#include "src/management/state_machine.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

using namespace myrd;
using namespace myrd::management;

constexpr std::size_t kHeaderBytes = 56;
constexpr std::size_t kDigestOffset = 24;
constexpr std::size_t kMinorOffset = 10;
constexpr std::size_t kTotalOffset = 12;
constexpr std::size_t kPayloadSizeOffset = 16;
// What the newer minor appended: one byte of slot and a thirty-two byte digest.
constexpr std::size_t kAppendedBytes = 33;

product::Uuid uuid(const char* text) {
    auto parsed = product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.value();
}

ProfileState ready_state() {
    return registered_profile_state(
        profile::ProfileLifecycle::ready,
        product::DiskState{
            product::RuntimeState::stopped,
            product::ActivityState::idle,
            product::PersistenceState::clean,
            product::HealthState::healthy});
}

Sha256Digest candidate_digest() {
    Sha256Digest value{};
    value.fill(std::byte{0xAB});
    return value;
}

// A record in the one phase that requires a correlation and forbids a
// disposition, so the correlation is exercised on the path that actually
// persists one.
DurableProfileStateRecord boundary_record() {
    DurableProfileStateRecord record{};
    record.revision = 5;
    record.state = ready_state();
    record.operation_gate = idle_operation_gate();
    record.checkpoint.confirmed_generation = 4;
    record.checkpoint.current_dirty_epoch = 11;
    record.checkpoint.phase = CheckpointDurablePhase::commit_boundary;
    record.checkpoint.before_state = ready_state();
    record.checkpoint.correlation = CheckpointCommitBoundaryEvidence{
        uuid("99999999-8888-7777-6666-555555555555"),
        uuid("11111111-2222-3333-4444-555555555555"),
        uuid("66666666-7777-8888-9999-aaaaaaaaaaaa"),
        4242,
        0x0123456789ABCDEFull,
        7,
        4,
        5,
        11,
        3903,
        2,
        candidate_digest()};
    return record;
}

void write_u32(std::vector<std::byte>& bytes, const std::size_t at,
               const std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index) {
        bytes[at + index] =
            static_cast<std::byte>((value >> (8U * index)) & 0xFFU);
    }
}

// Rewrites a record as the previous schema minor would have written it: the
// minor says 0, the two appended fields are gone, and the length fields and the
// payload digest are recomputed to match.  Hand-built on purpose -- only the old
// writer could produce one, and the point is to check the reader against bytes
// it will really meet.
std::vector<std::byte> as_previous_minor(std::vector<std::byte> record) {
    // The correlation is *not* the last thing in the payload -- the disposition
    // and the failure records follow it -- so the appended bytes are found by
    // their own value rather than by truncating the end.  The slot is a small
    // number and the digest is thirty-two 0xAB bytes, so the pair is distinctive
    // enough to occur exactly once.
    std::vector<std::byte> appended;
    appended.push_back(std::byte{2});
    for (int index = 0; index != 32; ++index) appended.push_back(std::byte{0xAB});
    const auto found = std::search(record.begin() + kHeaderBytes, record.end(),
                                   appended.begin(), appended.end());
    REQUIRE(found != record.end());
    record.erase(found, found + static_cast<std::ptrdiff_t>(appended.size()));

    record[kMinorOffset] = std::byte{0};
    record[kMinorOffset + 1] = std::byte{0};
    const auto total = static_cast<std::uint32_t>(record.size());
    write_u32(record, kTotalOffset, total);
    write_u32(record, kPayloadSizeOffset, total - kHeaderBytes);
    const auto digest =
        sha256_digest(std::span(record).subspan(kHeaderBytes));
    REQUIRE(digest.ok());
    std::copy(digest.value().begin(), digest.value().end(),
              record.begin() + kDigestOffset);
    return record;
}

} // namespace

TEST_CASE(m5_registration_codec_round_trips_the_recovery_fields) {
    // The slot and the digest are what a recovery proof judges against, so a
    // round trip that lost either would make every interrupted checkpoint
    // unprovable after a restart -- silently, and only on the machine that
    // actually restarted.
    const auto record = boundary_record();
    const auto encoded = encode_profile_state_record(record);
    REQUIRE(encoded.ok());

    const auto decoded = decode_profile_state_record(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value() == record);
    REQUIRE(decoded.value().checkpoint.correlation.has_value());
    const auto& correlation = *decoded.value().checkpoint.correlation;
    REQUIRE_EQ(correlation.manifest_slot, 2);
    REQUIRE(correlation.candidate_bytes_digest == candidate_digest());
    REQUIRE(correlation.recovery_recoverable());
}

TEST_CASE(m5_registration_codec_reads_a_record_from_the_previous_minor) {
    // A record written before the widening must still load.  Rejecting it would
    // turn a format change into data loss.
    const auto current = encode_profile_state_record(boundary_record());
    REQUIRE(current.ok());
    const auto legacy = as_previous_minor(current.value());

    const auto decoded = decode_profile_state_record(legacy);
    REQUIRE(decoded.ok());
    REQUIRE(decoded.value().checkpoint.correlation.has_value());
    const auto& correlation = *decoded.value().checkpoint.correlation;
    // The fields it never carried are absent, not invented -- and this is the
    // assertion the whole migration turns on.
    REQUIRE(!correlation.recovery_recoverable());
    REQUIRE_EQ(correlation.manifest_slot, 0);
    REQUIRE(correlation.candidate_bytes_digest == Sha256Digest{});
    // Everything it *did* carry survives, so the record is still useful.
    REQUIRE_EQ(correlation.prior_generation, 4ull);
    REQUIRE_EQ(correlation.candidate_generation, 5ull);
    REQUIRE(correlation.native_m3_operation_id.has_value());
    REQUIRE_EQ(*correlation.native_m3_operation_id, 3903ull);
}

TEST_CASE(m5_registration_codec_refuses_a_minor_from_the_future) {
    // Forward compatibility is not offered, and must not be: a minor this build
    // does not know may have appended fields, and reading the payload as if it
    // ended where this build expects would decode trailing bytes into whichever
    // field came next.
    auto bytes = encode_profile_state_record(boundary_record());
    REQUIRE(bytes.ok());
    auto record = bytes.move_value();
    record[kMinorOffset] = std::byte{0};
    record[kMinorOffset + 1] = std::byte{9};
    // The header checksum covers the payload, not the header, so the minor can
    // be rewritten without re-signing anything.
    REQUIRE(!decode_profile_state_record(record).ok());
}
