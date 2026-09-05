#pragma once

#include "src/common/result.h"
#include "src/management/service_core.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace myrd::management {

inline constexpr std::uint32_t kProfileRegistrationMaximumBytes =
    256U * 1024U;

struct DurableProfileStateRecord final {
    std::uint64_t revision{};
    ProfileState state;
    OperationGateState operation_gate;
    CheckpointDurableState checkpoint;
    CompactionDurableState compaction;
    StopDurableState stop;

    bool operator==(const DurableProfileStateRecord&) const noexcept = default;
};

bool validate_durable_profile_state_record(
    const DurableProfileStateRecord& record) noexcept;

enum class ProfileRegistrationCodecError : std::uint32_t {
    invalid_size = 0x5A01,
    invalid_magic,
    unsupported_schema,
    invalid_header,
    checksum_mismatch,
    invalid_fields,
    text_encoding,
};

Result<std::vector<std::byte>> encode_profile_state_record(
    const DurableProfileStateRecord& record);
Result<DurableProfileStateRecord> decode_profile_state_record(
    std::span<const std::byte> bytes);

Result<std::vector<std::byte>> encode_recovery_command_reservation(
    const RecoveryCommandReservation& record);
Result<RecoveryCommandReservation> decode_recovery_command_reservation(
    std::span<const std::byte> bytes);

Result<std::vector<std::byte>> encode_forget_tombstone(
    const ForgetTombstone& record);
Result<ForgetTombstone> decode_forget_tombstone(
    std::span<const std::byte> bytes);

} // namespace myrd::management
