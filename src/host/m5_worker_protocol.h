#pragma once

#include "src/common/m5_native_operation_id.h"
#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/product/uuid.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace myrd::m5 {

// The private service <-> per-profile worker protocol.
//
// This is deliberately not the public pipe protocol: the public pipe carries
// product commands and never reaches a bundle, while every message here is
// addressed to one already-bound worker and carries that binding.  It is also
// deliberately not `host_ipc`/`host_protocol`: those belong to the HostCore
// engine this implementation replaces, and the transfer manifest prohibits
// importing them.  Nothing below is copied from them.
//
// The transport is a byte stream; this header owns the typed messages and their
// strict codec.  Two rules make the channel trustworthy:
//
//   1. Every request and reply carries an authenticated `WorkerBinding` and the
//      product operation id it belongs to.  A message without them, or with a
//      binding that is not the one this session established, is refused.
//   2. `sequence` is strictly increasing per session.  A repeated or lower
//      sequence is a replay and is refused rather than re-executed.

inline constexpr std::uint16_t kWorkerProtocolMajor = 1;
inline constexpr std::uint16_t kWorkerProtocolMinor = 0;
// One prepared candidate carries a digest and small integer fields only, so the
// bound is comfortably above every message this protocol defines.
inline constexpr std::uint32_t kMaxWorkerMessageBytes = 64U * 1024U;

enum class WorkerMessageKind : std::uint16_t {
    request = 1,
    reply = 2,
};

enum class WorkerCommand : std::uint16_t {
    handshake = 1,
    start = 2,
    status = 3,
    checkpoint_prepare = 4,
    checkpoint_commit = 5,
    checkpoint_abort = 6,
    runtime_observation = 7,
    recovery_proof = 8,
    cleanup = 9,
};

// Identity of one worker process bound to one profile for one session.
//
// `process_creation_time` is the Win32 process creation FILETIME.  A pid alone
// is not an identity: Windows reuses pids, so a dead worker and its replacement
// can share one.  Pid plus creation time is what makes "the same worker" a
// question with an answer, and `session_generation` makes it specific to this
// service session rather than the previous one.
struct WorkerBinding final {
    std::uint32_t process_id{};
    std::uint64_t process_creation_time{};
    std::uint64_t session_generation{};
    product::Uuid profile_id;

    bool operator==(const WorkerBinding&) const noexcept = default;
};

struct WorkerHandshake final {
    std::uint16_t protocol_major{kWorkerProtocolMajor};
    std::uint16_t protocol_minor{kWorkerProtocolMinor};
    // A value the service generates once per session.  Echoing it proves the
    // peer answered *this* session rather than replaying an earlier handshake.
    std::uint64_t session_nonce{};
};

// What prepare freezes.  The service persists exactly this as its P4
// correlation, and commit must repeat the native id, the slot and the digest
// before the candidate may be published.
struct WorkerPreparedCandidate final {
    std::uint64_t prior_generation{};
    std::uint64_t candidate_generation{};
    std::uint64_t native_operation_id{};
    std::uint64_t cut_dirty_epoch{};
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};

    bool operator==(const WorkerPreparedCandidate&) const noexcept = default;
};

struct WorkerCheckpointCommit final {
    std::uint64_t native_operation_id{};
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};

    bool operator==(const WorkerCheckpointCommit&) const noexcept = default;
};

// The worker's own view of what it is running.  Mirrors the service-side
// WorkerRuntimeObservation, but is defined here so `src/host` never has to
// include `src/management`: the service adapter maps one onto the other.
struct WorkerRuntimeStatus final {
    bool worker_alive{};
    std::uint64_t committed_generation{};
    bool dirty{};
    std::uint64_t dirty_epoch{};
    std::optional<product::Uuid> active_operation_id;
    std::uint32_t active_operation_phase{};

    bool operator==(const WorkerRuntimeStatus&) const noexcept = default;
};

// Exactly three answers, and the third is not a failure to be retried: a
// generation that changed but cannot be attributed to this operation must never
// be promoted to a success or rolled back to a "not committed".
enum class WorkerRecoveryOutcome : std::uint8_t {
    ownership_unprovable = 0,
    prior_generation_proved,
    candidate_generation_proved,
};

struct WorkerRecoveryProof final {
    WorkerRecoveryOutcome outcome{WorkerRecoveryOutcome::ownership_unprovable};
    std::uint64_t observed_generation{};
    std::uint64_t native_operation_id{};
    std::uint8_t manifest_slot{};
    Sha256Digest candidate_bytes_digest{};

    bool operator==(const WorkerRecoveryProof&) const noexcept = default;
};

// One typed private message.
//
// `payload` selects which optional is meaningful, and the codec refuses any
// message that carries a payload its command does not define.  Stray fields are
// an error, not something to ignore: silently dropping a field is how a
// mismatched digest would stop being checked.
struct WorkerMessage final {
    WorkerMessageKind kind{WorkerMessageKind::request};
    WorkerCommand command{WorkerCommand::handshake};
    std::uint64_t sequence{};
    std::optional<WorkerBinding> binding;
    std::optional<product::Uuid> product_operation_id;
    std::optional<WorkerHandshake> handshake;
    std::optional<WorkerPreparedCandidate> prepared;
    std::optional<WorkerCheckpointCommit> commit;
    std::optional<WorkerRuntimeStatus> status;
    std::optional<WorkerRecoveryProof> recovery;
    // Set only on a reply that reports failure.  A reply is either a payload or
    // an error, never both.
    std::optional<Error> error;
};

[[nodiscard]] Result<std::vector<std::byte>> encode_worker_message(
    const WorkerMessage& message);

// Structural decoding only.  This fails closed on a truncated, oversized, or
// self-inconsistent message, but it has no opinion about whether the message is
// authentic for any particular session -- that is `authenticate_worker_message`.
[[nodiscard]] Result<WorkerMessage> decode_worker_message(
    std::span<const std::byte> bytes);

// The authentication gate.  Refuses a missing or mismatched binding, a message
// that does not carry the operation id its command requires, a sequence that is
// not strictly greater than `last_accepted_sequence` (a replay), and any
// command/payload combination the protocol does not define.
[[nodiscard]] Result<void> authenticate_worker_message(
    const WorkerMessage& message, const WorkerBinding& expected_binding,
    std::uint64_t last_accepted_sequence);

// The worker-side gate before publishing: commit must repeat exactly the
// candidate that prepare froze.  A mismatch leaves the candidate prepared so
// the service can retry the correct identity or abort; it never publishes.
[[nodiscard]] Result<void> require_matching_candidate(
    const WorkerCheckpointCommit& commit,
    const WorkerPreparedCandidate& prepared);

} // namespace myrd::m5
