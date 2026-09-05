#include "test_framework.h"

#include "src/management/profile_registration_codec.h"
#include "src/common/sha256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

using namespace myrd::management;

namespace {

constexpr std::size_t kRecordHeaderBytes = 56;
constexpr std::size_t kRecordDigestOffset = 24;
constexpr std::size_t kEncodedUuidBytes = sizeof(std::uint32_t) + 16;

constexpr std::size_t state_runtime_offset() {
    return kRecordHeaderBytes + sizeof(std::uint64_t) +
        sizeof(std::uint8_t) + sizeof(std::uint8_t);
}

constexpr std::size_t state_gate_presence_offset() {
    constexpr std::size_t kEncodedProfileStateBytes =
        sizeof(std::uint8_t) + sizeof(std::uint8_t) + 4 * sizeof(std::uint8_t);
    return kRecordHeaderBytes + sizeof(std::uint64_t) +
        kEncodedProfileStateBytes;
}

constexpr std::size_t reservation_kind_offset() {
    return kRecordHeaderBytes + 2 * kEncodedUuidBytes;
}

constexpr std::size_t tombstone_operation_uuid_offset() {
    return kRecordHeaderBytes + sizeof(std::uint32_t);
}

myrd::product::Uuid uuid(std::string_view text) {
    auto parsed = myrd::product::Uuid::parse(text);
    REQUIRE(parsed.ok());
    return parsed.move_value();
}

DurableProfileStateRecord minimal_state_record() {
    DurableProfileStateRecord record;
    record.revision = 1;
    record.state = registered_profile_state(
        myrd::profile::ProfileLifecycle::ready,
        myrd::product::DiskState{
            myrd::product::RuntimeState::stopped,
            myrd::product::ActivityState::idle,
            myrd::product::PersistenceState::clean,
            myrd::product::HealthState::healthy});
    return record;
}

myrd::profile::ProfileRecord expected_profile() {
    using namespace myrd::profile;
    Profile value{
        uuid("11111111-1111-4111-8111-111111111111"), L"S-1-5-18",
        L"registration", 1'700'000'000'000ULL, ProfileLifecycle::ready,
        L"D:\\disk.rdm", "disk.rdm",
        uuid("22222222-2222-4222-8222-222222222222"), 8ULL << 30, 512,
        4096, uuid("33333333-3333-4333-8333-333333333333"),
        uuid("44444444-4444-4444-8444-444444444444"),
        uuid("55555555-5555-4555-8555-555555555555"),
        MountKind::drive_letter, L"R:", true,
        MemoryPolicy{MemoryMode::automatic, 1ULL << 30, 896ULL << 20,
                     768ULL << 20, 128ULL << 20},
        CheckpointPolicy{}, CompactionPolicy{}};
    return ProfileRecord{9, std::move(value)};
}

RecoveryCommandReservation reservation() {
    const auto profile = expected_profile();
    return RecoveryCommandReservation{
        uuid("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        uuid("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
        RecoveryOperationIntent{RecoveryOperationKind::rebind,
            profile.profile.profile_uuid, L"D:\\disk.rdm",
            profile.profile.image_uuid, std::nullopt, false},
        minimal_state_record().state, profile};
}

RecoveryCommandReservation repair_reservation() {
    auto value = reservation();
    value.intent = RecoveryOperationIntent{RecoveryOperationKind::repair_profile,
        value.expected_profile.profile.profile_uuid, L"", std::nullopt,
        std::nullopt, true};
    return value;
}

RecoveryCommandReservation forget_reservation() {
    auto value = reservation();
    value.intent = RecoveryOperationIntent{RecoveryOperationKind::forget,
        value.expected_profile.profile.profile_uuid, L"", std::nullopt,
        value.expected_profile.profile.profile_uuid, false};
    return value;
}

ForgetTombstone tombstone() {
    BundleFileIdentity identity{77, {}};
    identity.file_id.fill(std::byte{0x5a});
    return ForgetTombstone{
        uuid("aaaaaaaa-0000-4000-8000-000000000001"),
        uuid("aaaaaaaa-0000-4000-8000-000000000002"),
        uuid("aaaaaaaa-0000-4000-8000-000000000003"),
        uuid("aaaaaaaa-0000-4000-8000-000000000004"), 12, 34,
        L"D:\\disk.rdm", L"S-1-5-18", identity,
        minimal_state_record().state, ForgetTombstonePhase::removal_started};
}

DurableProfileStateRecord maximal_state_record() {
    auto value = minimal_state_record();
    value.revision = 99;
    value.operation_gate = OperationGateState{
        uuid("90000000-0000-4000-8000-000000000001"),
        ManagementCommand::checkpoint, true};
    value.checkpoint = CheckpointDurableState{
        20, 21, CheckpointDurablePhase::resolving, value.state,
        CheckpointCommitBoundaryEvidence{
            uuid("90000000-0000-4000-8000-000000000002"),
            uuid("90000000-0000-4000-8000-000000000003"),
            uuid("90000000-0000-4000-8000-000000000004"),
            42, 43, 44, 45, 46, 47, 48},
        CheckpointCommitDisposition::committed_needs_reconciliation,
        CheckpointFailureEvidence{myrd::ErrorDomain::persistence, 100, L"检查点",
            L"failure message", 101,
            CheckpointPersistenceErrorContext{"commit", 102, 3, 103, 104,
                {}, 105, CheckpointCommitDisposition::commit_indeterminate,
                "safe-context"}},
        CheckpointReconciliationFailureEvidence{myrd::ErrorDomain::internal, 106,
            L"reconcile", L"message", 107,
            CheckpointCommitDisposition::committed, "context"}};
    value.checkpoint.failure->context.image_uuid.fill(std::byte{0x11});

    CompactionMetrics metrics{};
    metrics.source_generation = 1; metrics.committed_generation = 2;
    metrics.source_base_bytes = 3; metrics.source_journal_bytes = 4;
    metrics.candidate_base_bytes = 5; metrics.candidate_journal_bytes = 6;
    metrics.required_peak_free_bytes = 7; metrics.available_free_bytes = 8;
    metrics.live_extent_count = 9; metrics.candidate_payloads = 10;
    metrics.snapshot_records = 11; metrics.data_records = 12;
    metrics.zero_records = 13; metrics.discard_records = 14;
    metrics.index_nodes = 15; metrics.newer_dirty_preserved = 16;
    metrics.retained_candidate_count = 17; metrics.deleted_candidate_count = 18;
    value.compaction = CompactionDurableState{
        CompactionDurablePhase::product_truth_published, value.state,
        CompactionMutationBoundaryEvidence{
            uuid("91000000-0000-4000-8000-000000000001"),
            uuid("91000000-0000-4000-8000-000000000002"),
            uuid("91000000-0000-4000-8000-000000000003"),
            1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, true},
        CompactionCommitDisposition::committed_needs_reconciliation, metrics,
        CompactionFailureEvidence{myrd::ErrorDomain::storage, 200, L"compact",
            L"failed", 201,
            CompactionPersistenceErrorContext{"replace", 202, 4, 203, 204,
                {}, 205, CompactionCommitDisposition::committed,
                "compaction-context"}},
        CompactionReconciliationFailureEvidence{myrd::ErrorDomain::internal, 206,
            L"reconcile", L"failed", 207,
            CompactionCommitDisposition::commit_indeterminate, "safe"},
        CompactionPreflightFailureEvidence{
            uuid("92000000-0000-4000-8000-000000000001"),
            uuid("92000000-0000-4000-8000-000000000002"),
            uuid("92000000-0000-4000-8000-000000000003"),
            1, 2, 3, 4, true},
        true, 300, CheckpointCommitDisposition::committed, 301, 302,
        uuid("93000000-0000-4000-8000-000000000001"),
        uuid("93000000-0000-4000-8000-000000000002")};
    value.compaction.failure->context.image_uuid.fill(std::byte{0x22});
    value.stop = StopDurableState{StopDurablePhase::resolving,
        uuid("94000000-0000-4000-8000-000000000001"), value.state,
        L"R:", myrd::profile::NormalStopPolicy::require_clean,
        true, true, true, 400, 401};
    return value;
}

void put_u16(std::vector<std::byte>& bytes, std::size_t offset,
             std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xffU);
    bytes[offset + 1] = static_cast<std::byte>(value >> 8U);
}

void refresh_hash(std::vector<std::byte>& bytes) {
    auto digest = myrd::sha256_digest(
        std::span<const std::byte>(bytes).subspan(kRecordHeaderBytes));
    REQUIRE(digest.ok());
    std::copy(digest.value().begin(), digest.value().end(),
              bytes.begin() + kRecordDigestOffset);
}

template <class Decode>
void require_all_truncations_rejected(const std::vector<std::byte>& bytes,
                                      Decode decode) {
    for (std::size_t size = 0; size != bytes.size(); ++size) {
        REQUIRE(decode(std::span<const std::byte>(bytes).first(size)).has_error());
    }
}

} // namespace

TEST_CASE(profile_registration_state_default_round_trips) {
    const auto expected = minimal_state_record();
    const auto encoded = encode_profile_state_record(expected);
    REQUIRE(encoded.ok());
    const auto decoded = decode_profile_state_record(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), expected);
}

TEST_CASE(profile_registration_state_all_nested_fields_round_trip) {
    const auto expected = maximal_state_record();
    const auto encoded = encode_profile_state_record(expected);
    REQUIRE(encoded.ok());
    const auto decoded = decode_profile_state_record(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), expected);
}

TEST_CASE(profile_registration_reservation_round_trips_all_fields) {
    const auto expected = reservation();
    const auto encoded = encode_recovery_command_reservation(expected);
    REQUIRE(encoded.ok());
    const auto decoded = decode_recovery_command_reservation(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), expected);
}

TEST_CASE(profile_registration_repair_and_forget_reservations_round_trip) {
    const auto repair = repair_reservation();
    const auto repair_bytes = encode_recovery_command_reservation(repair);
    REQUIRE(repair_bytes.ok());
    REQUIRE_EQ(decode_recovery_command_reservation(repair_bytes.value()).value(),
               repair);

    const auto forget = forget_reservation();
    const auto forget_bytes = encode_recovery_command_reservation(forget);
    REQUIRE(forget_bytes.ok());
    REQUIRE_EQ(decode_recovery_command_reservation(forget_bytes.value()).value(),
               forget);
}

TEST_CASE(profile_registration_tombstone_round_trips_all_fields) {
    const auto expected = tombstone();
    const auto encoded = encode_forget_tombstone(expected);
    REQUIRE(encoded.ok());
    const auto decoded = decode_forget_tombstone(encoded.value());
    REQUIRE(decoded.ok());
    REQUIRE_EQ(decoded.value(), expected);
}

TEST_CASE(profile_registration_each_truncation_is_rejected) {
    const auto state = encode_profile_state_record(maximal_state_record());
    const auto reserve = encode_recovery_command_reservation(reservation());
    const auto forget = encode_forget_tombstone(tombstone());
    REQUIRE(state.ok()); REQUIRE(reserve.ok()); REQUIRE(forget.ok());
    require_all_truncations_rejected(state.value(), decode_profile_state_record);
    require_all_truncations_rejected(reserve.value(), decode_recovery_command_reservation);
    require_all_truncations_rejected(forget.value(), decode_forget_tombstone);
}

TEST_CASE(profile_registration_checksum_mutation_and_trailing_bytes_are_rejected) {
    const auto encoded = encode_profile_state_record(maximal_state_record());
    REQUIRE(encoded.ok());
    for (std::size_t i = kRecordHeaderBytes; i != encoded.value().size(); ++i) {
        auto mutated = encoded.value();
        mutated[i] ^= std::byte{0x01};
        REQUIRE(decode_profile_state_record(mutated).has_error());
    }
    auto trailing = encoded.value();
    trailing.push_back(std::byte{0});
    REQUIRE(decode_profile_state_record(trailing).has_error());
}

TEST_CASE(profile_registration_all_record_payload_mutations_are_rejected) {
    const auto reserve = encode_recovery_command_reservation(reservation());
    const auto forget = encode_forget_tombstone(tombstone());
    REQUIRE(reserve.ok()); REQUIRE(forget.ok());
    for (std::size_t i = kRecordHeaderBytes; i != reserve.value().size(); ++i) {
        auto mutated = reserve.value();
        mutated[i] ^= std::byte{0x80};
        REQUIRE(decode_recovery_command_reservation(mutated).has_error());
    }
    for (std::size_t i = kRecordHeaderBytes; i != forget.value().size(); ++i) {
        auto mutated = forget.value();
        mutated[i] ^= std::byte{0x40};
        REQUIRE(decode_forget_tombstone(mutated).has_error());
    }
}

TEST_CASE(profile_registration_record_magics_are_distinct) {
    const auto state = encode_profile_state_record(minimal_state_record());
    const auto reserve = encode_recovery_command_reservation(reservation());
    const auto forget = encode_forget_tombstone(tombstone());
    REQUIRE(state.ok()); REQUIRE(reserve.ok()); REQUIRE(forget.ok());
    REQUIRE(!std::equal(state.value().begin(), state.value().begin() + 8,
                        reserve.value().begin()));
    REQUIRE(!std::equal(state.value().begin(), state.value().begin() + 8,
                        forget.value().begin()));
    REQUIRE(decode_forget_tombstone(state.value()).has_error());
}

TEST_CASE(profile_registration_magic_major_and_semantic_enum_are_rejected) {
    const auto encoded = encode_profile_state_record(minimal_state_record());
    REQUIRE(encoded.ok());
    auto bad_magic = encoded.value();
    bad_magic[0] ^= std::byte{1};
    REQUIRE(decode_profile_state_record(bad_magic).has_error());
    auto bad_major = encoded.value();
    put_u16(bad_major, 8, 2);
    REQUIRE(decode_profile_state_record(bad_major).has_error());
    auto bad_minor = encoded.value();
    put_u16(bad_minor, 10, 1);
    const auto rejected_minor = decode_profile_state_record(bad_minor);
    REQUIRE(rejected_minor.has_error());
    REQUIRE_EQ(
        rejected_minor.error().code,
        static_cast<std::uint32_t>(
            ProfileRegistrationCodecError::unsupported_schema));
    auto bad_runtime = encoded.value();
    bad_runtime[state_runtime_offset()] = std::byte{0xff};
    refresh_hash(bad_runtime);
    REQUIRE(decode_profile_state_record(bad_runtime).has_error());

    auto bad_presence = encoded.value();
    bad_presence[state_gate_presence_offset()] = std::byte{2};
    refresh_hash(bad_presence);
    REQUIRE(decode_profile_state_record(bad_presence).has_error());

    const auto reserve = encode_recovery_command_reservation(reservation());
    REQUIRE(reserve.ok());
    auto bad_kind = reserve.value();
    bad_kind[reservation_kind_offset()] = std::byte{0xff};
    refresh_hash(bad_kind);
    REQUIRE(decode_recovery_command_reservation(bad_kind).has_error());

    const auto forget = encode_forget_tombstone(tombstone());
    REQUIRE(forget.ok());
    auto bad_phase = forget.value();
    bad_phase.back() = std::byte{0xff};
    refresh_hash(bad_phase);
    REQUIRE(decode_forget_tombstone(bad_phase).has_error());
}

TEST_CASE(profile_registration_reservation_kind_contracts_are_enforced) {
    auto repair = repair_reservation();
    repair.intent.bundle_path = L"D:\\unexpected.rdm";
    REQUIRE(encode_recovery_command_reservation(repair).has_error());

    auto rebind = reservation();
    rebind.intent.expected_image_id =
        uuid("ffffffff-ffff-4fff-8fff-ffffffffffff");
    REQUIRE(encode_recovery_command_reservation(rebind).has_error());

    auto forget = forget_reservation();
    forget.intent.confirmation_id =
        uuid("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    REQUIRE(encode_recovery_command_reservation(forget).has_error());

    auto mismatch = reservation();
    mismatch.before_state.lifecycle = myrd::profile::ProfileLifecycle::provisioning;
    REQUIRE(encode_recovery_command_reservation(mismatch).has_error());
}

TEST_CASE(profile_registration_rejects_oversized_and_invalid_utf16_text) {
    auto over = maximal_state_record();
    over.stop.target.assign(32U * 1024U + 1U, L'a');
    REQUIRE(encode_profile_state_record(over).has_error());
    auto invalid = tombstone();
    invalid.retained_bundle_path.assign(1, static_cast<wchar_t>(0xd800));
    REQUIRE(encode_forget_tombstone(invalid).has_error());

    auto invalid_utf8 = maximal_state_record();
    invalid_utf8.checkpoint.failure->context.operation =
        std::string("\xc0\xaf", 2);
    REQUIRE(encode_profile_state_record(invalid_utf8).has_error());

    auto nul = maximal_state_record();
    nul.compaction.failure->context.safe_context = std::string("a\0b", 3);
    REQUIRE(encode_profile_state_record(nul).has_error());
}

TEST_CASE(profile_registration_rejects_zero_identities_and_invalid_gate) {
    auto bad_gate = minimal_state_record();
    bad_gate.operation_gate.active_command = ManagementCommand::checkpoint;
    REQUIRE(encode_profile_state_record(bad_gate).has_error());

    auto bad_identity = tombstone();
    bad_identity.retained_bundle_identity->volume_serial = 0;
    REQUIRE(encode_forget_tombstone(bad_identity).has_error());

    auto missing_identity = tombstone();
    missing_identity.retained_bundle_identity.reset();
    REQUIRE(encode_forget_tombstone(missing_identity).has_error());

    const auto encoded = encode_forget_tombstone(tombstone());
    REQUIRE(encoded.ok());
    auto zero_operation = encoded.value();
    const auto operation_uuid = tombstone_operation_uuid_offset();
    std::fill(zero_operation.begin() + static_cast<std::ptrdiff_t>(operation_uuid),
              zero_operation.begin() + static_cast<std::ptrdiff_t>(operation_uuid + 16),
              std::byte{0});
    refresh_hash(zero_operation);
    REQUIRE(decode_forget_tombstone(zero_operation).has_error());
}

TEST_CASE(profile_registration_rejects_phase_evidence_contradictions) {
    auto stable_stop = minimal_state_record();
    stable_stop.stop.operation_id =
        uuid("95000000-0000-4000-8000-000000000001");
    REQUIRE(encode_profile_state_record(stable_stop).has_error());

    auto boundary_without_identity = minimal_state_record();
    boundary_without_identity.stop.phase = StopDurablePhase::removal_boundary;
    REQUIRE(encode_profile_state_record(boundary_without_identity).has_error());

    auto committed_without_proof = minimal_state_record();
    committed_without_proof.checkpoint.phase =
        CheckpointDurablePhase::committed_pending_epoch_confirmation;
    committed_without_proof.checkpoint.before_state =
        committed_without_proof.state;
    REQUIRE(encode_profile_state_record(committed_without_proof).has_error());

    auto mutation_without_proof = minimal_state_record();
    mutation_without_proof.compaction.phase =
        CompactionDurablePhase::mutation_boundary;
    mutation_without_proof.compaction.before_state = mutation_without_proof.state;
    REQUIRE(encode_profile_state_record(mutation_without_proof).has_error());
}
