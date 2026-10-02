#pragma once

#include "src/common/m5_native_operation_id.h"
#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/persistence/image_inspector.h"
#include "src/product/uuid.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
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
    require_runtime_observation = 7,
    recovery_proof = 8,
    remove_and_stop = 9,
    shutdown = 10,
    cleanup = 11,
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

// The service's authorization to remove one profile's resources.
//
// Design spec §5 permits a worker to clean only what it has *verified it owns*,
// and the mandatory gate forbids reporting a removal without proof of the writer
// lock, the matching device and the retained bundle identity.  So this carries
// the proof rather than a permission flag, and the three proofs stay three
// separate booleans: they are obtained separately, and folding them into one
// "safe" bit is exactly how a partial proof starts reading as a complete one.
struct WorkerRemovalAuthorization final {
    // The two ids have no default constructor, so they are constructor
    // parameters rather than fields a caller could leave unset: an
    // authorization that names no operation or no profile is not something to
    // build by accident.
    WorkerRemovalAuthorization(product::Uuid removal_operation_id,
                               product::Uuid profile_id) noexcept
        : removal_operation_id(std::move(removal_operation_id)),
          profile_id(std::move(profile_id)) {}

    // The product operation that asked for the removal, so a refusal can name
    // it in the durable record.
    product::Uuid removal_operation_id;
    product::Uuid profile_id;
    persistence::ImageUuid image_id{};
    // Digest of the durable safety evidence the service obtained.  The worker
    // does not interpret it; carrying it means a refusal and the service's own
    // record can be compared afterwards.
    Sha256Digest ownership_evidence_digest{};
    bool writer_lock_proved{};
    bool device_match_proved{};
    bool bundle_identity_proved{};

    bool operator==(const WorkerRemovalAuthorization&) const noexcept = default;
};

// The worker's own view of what it is running.  Mirrors the service-side
// WorkerRuntimeObservation, but is defined here so `src/host` never has to
// include `src/management`: the service adapter maps one onto the other.
struct WorkerRuntimeStatus final {
    bool worker_alive{};
    // Which bundle this status is about.  It is here because a removal
    // authorization has to be checked against the image the worker actually
    // holds, and the only honest source for that is the worker's own
    // observation rather than the authorization itself.
    persistence::ImageUuid image_id{};
    std::uint64_t committed_generation{};
    bool dirty{};
    std::uint64_t dirty_epoch{};
    std::optional<product::Uuid> active_operation_id;
    std::uint32_t active_operation_phase{};

    bool operator==(const WorkerRuntimeStatus&) const noexcept = default;
};

// The P4 durable correlation, as the service persists it, handed to a fresh
// worker so it can decide what actually reached the bundle (design spec 3.5).
//
// The originating worker's in-process transaction died with it, so nothing can
// be aborted or resumed.  The only honest answers are the three below, and they
// are derived from what is on disk rather than from what the dead worker
// claimed: the journal native id plus the image and generation identities.
struct WorkerRecoveryCorrelation final {
    // The product operation the proof is about.  It is the service's own
    // bookkeeping id and has no persistence-layer counterpart, so it lives here.
    product::Uuid product_operation_id;
    // Everything the bundle itself must be matched against, in the persistence
    // layer's own type.  The read-side inspection consumes this struct
    // unchanged, so the wire format and the inspection API cannot drift into
    // two field sets that have to be kept in sync by hand.
    persistence::CheckpointRecoveryCorrelation checkpoint;

    bool operator==(const WorkerRecoveryCorrelation&) const noexcept = default;
};

// Exactly three answers, and the third is not a failure to be retried: a
// generation that changed but cannot be attributed to this operation must never
// be promoted to a success or rolled back to a "not committed".
//
// This is the protocol's own spelling of the three states, kept separate from
// `persistence::CheckpointRecoveryVerdict` so that a wire enum cannot silently
// change meaning when the persistence enum grows a fourth case.  The two are
// related by exactly one function, `worker_recovery_outcome()`, so they cannot
// drift in more than one place.
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
    // Carried by a recovery-proof request: what the service durably recorded at
    // P4, which is the only thing a fresh worker may judge ownership against.
    std::optional<WorkerRecoveryCorrelation> correlation;
    // Carried by a remove-and-stop request: the service's ownership proof.
    // Without it the worker has nothing to remove *as its own* and must refuse.
    std::optional<WorkerRemovalAuthorization> removal;
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

// The removal gate.  A removal is refused unless every part of the ownership
// proof is present and the authorization names this worker's own profile and
// image.  A refusal means the caller keeps the registration *and* the bundle and
// must not report success -- there is no partial removal to fall back to.
[[nodiscard]] Result<void> require_removal_authorized(
    const WorkerRemovalAuthorization& authorization,
    const WorkerBinding& binding,
    const persistence::ImageUuid& observed_image_id);

// The one mapping between the persistence verdict and this protocol's spelling
// of it.  A verdict the protocol has no case for is `ownership_unprovable`,
// which is the fail-closed direction: an unrecognised answer from the read side
// must never be read as a proof.
[[nodiscard]] WorkerRecoveryOutcome worker_recovery_outcome(
    persistence::CheckpointRecoveryVerdict verdict) noexcept;

} // namespace myrd::m5
