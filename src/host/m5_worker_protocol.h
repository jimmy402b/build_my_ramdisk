#pragma once

#include "src/common/m5_native_operation_id.h"
#include "src/common/result.h"
#include "src/common/sha256.h"
#include "src/persistence/image_inspector.h"
#include "src/product/uuid.h"

#include <array>
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
// 0 -> 1: `WorkerRuntimeStatus` gained `writes_frozen`, so the status payload
// changed length.  Both ends of this channel are built together and the
// handshake requires an exact match, so bumping the minor is what makes a stale
// worker fail the handshake instead of decoding a status one byte short and
// reporting a plausible runtime that was never observed.
inline constexpr std::uint16_t kWorkerProtocolMinor = 1;
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
    // Brings up the runtime this worker was launched for and answers with a
    // status whose bundle evidence is engaged.  It exists because the service
    // has to register a bundle *before* it can publish the durable profile the
    // worker would otherwise need to be launched from, so this is the one
    // command that is sent to a worker launched from a lifecycle intent rather
    // than from a durable record.
    //
    // It lands on the same runtime call `start` does -- the runtime offers one
    // granularity, and a bundle with no volume checkpoint cannot be recovered --
    // but it promises something `start` does not: that the bundle's identity was
    // recorded, which is what makes a registration possible at all.
    create_bundle = 12,
    // Opens the bundle read-only and answers with a status whose bundle evidence
    // is engaged, without starting a runtime at all.
    //
    // It is a separate command rather than a mode of `create_bundle` because the
    // two are not the same operation wearing different hats: `create_bundle`
    // runs the create lifecycle, and this one runs no lifecycle.  An import
    // registers a bundle that already exists and is not started, and the only
    // other way to open a bundle in this runtime -- `DiskRuntime::recover` --
    // assigns a drive letter and mounts a volume the import never asked for.
    // The action is named read-only, so the command is.
    inspect_bundle = 13,
    // Takes the exact volume's exclusive lock and *keeps* it, so the mounted
    // filesystem cannot accept another write until the runtime that holds it
    // either dismantles the volume or releases it.
    //
    // It exists as its own command rather than as a mode of `remove_and_stop`
    // because the window it opens has to cover three separate commands.  The
    // stop plan is `quiesce -> freeze -> flush -> checkpoint -> remove`, and only
    // the last of those dismantles anything; a freeze taken inside the removal
    // would leave the commit it is supposed to protect outside its own window,
    // which is exactly the defect this closes.  The lock therefore lives in the
    // worker's runtime, which is the only place that survives across commands.
    //
    // It is also deliberately not folded into `quiesce_mutations`.  Quiescing
    // there is a *proof* -- that the runtime being stopped is the live one the
    // durable profile describes -- and the comment at that adapter records the
    // real constraint that it holds no write freeze to take.  Making quiescing
    // take the lock would erase the distinction between "I verified what is
    // running" and "I stopped it from changing", and the two have different
    // failure meanings: a failed proof means the operation was aimed at the
    // wrong runtime, a failed freeze means the volume is busy and the stop must
    // be retried rather than forced.
    freeze_writes = 14,
    // Releases the write freeze `freeze_writes` took, without dismantling
    // anything.
    //
    // It is here because the stop plan can be rolled back: a flush or a commit
    // that fails leaves a profile that is still supposed to be running, and a
    // freeze nobody releases would leave that profile mounted and unwritable
    // until its worker died -- a worse outcome than the open write window this
    // whole increment exists to close.  Dismantling is not an acceptable
    // substitute, because the profile is not being stopped any more.
    release_write_freeze = 15,
    // Dismantles a volume this service can prove is its own, left behind by a worker
    // that was killed rather than asked to leave.
    //
    // **Why this cannot be an existing command.**  A `TerminateProcess`ed worker never
    // runs its ordered teardown, so it leaves a mounted volume and an assigned drive
    // letter that no live runtime owns.  The teardown itself already exists --
    // `DiskRuntime::shutdown_clean` removes the letter, dismounts and stops AIM -- but
    // it needs an *open* volume, and `adopt_existing` **mounts**.  Mounting is not
    // dismantling, and here it cannot even succeed: the residue is holding the very
    // drive letter the mount would ask for.
    //
    // So this command's whole job is to open the recorded volume **for dismantling**:
    // find it by the identities the service proved, take it without assigning anything,
    // run the teardown, and answer whether the letter and the volume are gone.
    //
    // It is appended rather than inserted, and that is the protocol's own convention:
    // the values are on the wire, so inserting one would silently reinterpret every
    // command after it.
    reclaim_leftover = 16,
    // Writes the bundle's durable image to a standalone VHDX file the request
    // names.  Read-only with respect to the bundle: no runtime is started and no
    // manifest, base or journal is written.  Appended for the same wire reason as
    // every command above.
    export_bundle = 17,
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

// A value the service generates once per worker session and delivers *only*
// through the worker's inherited startup channel, never on a command line and
// never in a file.  Presenting it proves the peer read that channel, which is
// what distinguishes "the worker I launched" from "anyone who can reach the pipe
// name" -- the pipe ACL is the other half, and neither half is sufficient alone.
struct StartupSecret final {
    std::array<std::byte, 32> bytes{};

    static Result<StartupSecret> generate();
    bool operator==(const StartupSecret&) const noexcept = default;
};

struct WorkerHandshake final {
    std::uint16_t protocol_major{kWorkerProtocolMajor};
    std::uint16_t protocol_minor{kWorkerProtocolMinor};
    // A value the service generates once per session.  Echoing it proves the
    // peer answered *this* session rather than replaying an earlier handshake.
    std::uint64_t session_nonce{};
    // The startup secret from the inherited channel.  A handshake without it,
    // or with another session's, is refused.
    StartupSecret startup_secret{};
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

// What a commit actually did.  The persistence layer's `CommitDisposition`
// restated in the protocol's own enum, for the same reason
// `WorkerRecoveryOutcome` restates its verdict: a wire enum must not silently
// change meaning when the persistence enum grows a case.  The two are related by
// exactly one function, `worker_commit_disposition()`, so they cannot drift in
// more than one place.
//
// The four cases are four *different* answers and must not be folded.  In
// particular `committed_needs_reconciliation` is not `commit_indeterminate`: the
// first says the bytes landed and the epoch publication did not, the second says
// the outcome is unknown.  Folding them told the service to park an operation
// whose recovery path was known, and the safe answer for one is the unsafe
// answer for the other.
enum class WorkerCommitDisposition : std::uint8_t {
    // Zero is the fail-closed default: a disposition that was never set reads as
    // "nothing was published", which is the answer that authorizes no promotion.
    not_committed = 0,
    commit_indeterminate,
    committed,
    committed_needs_reconciliation,
};

// The persistence failure that decided a commit which did not cleanly land,
// relayed field for field.  ServiceCore validates its own evidence against this
// record, so a summary -- or a context recomposed from parts -- would name a
// persistence failure that never happened.
struct WorkerCommitFailure final {
    std::string operation;
    std::uint32_t code{};
    std::uint8_t phase{};
    std::uint32_t native_code{};
    std::uint64_t native_operation_id{};
    persistence::ImageUuid image_uuid{};
    std::uint64_t generation{};
    std::string safe_context;

    bool operator==(const WorkerCommitFailure&) const noexcept = default;
};

// `WorkerCheckpointCommitReply` is declared below `WorkerRuntimeStatus`: its
// answer carries an observation, so it cannot precede the type it observes.

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
    // **The caller has accepted losing writes the removal will drop.**  A force
    // stop carries this because the user passed `--accept-volatile-data-loss`;
    // without it the runtime keeps its fail-closed refusal for a dirty runtime.
    // It authorizes discarding data, not detecting it: the dirty check still
    // runs and still flushes before deciding.
    bool accept_data_loss{};

    bool operator==(const WorkerRemovalAuthorization&) const noexcept = default;
};

// The persistent GPT identities of the volume the worker's own runtime
// adopted, in canonical lowercase GUID text.
//
// Text rather than raw bytes on purpose.  These are compared against the
// durable profile's identities and re-parsed by the service into
// `product::Uuid`, and canonical text is the one form both ends can produce and
// check with the shared parser -- sixteen raw bytes would be a second encoding
// that has to be kept in step with it by hand.
//
// `complete()` is the gate, not a formatting convenience.  A worker that read
// two of three identities has an identity that cannot be compared against
// anything, so the service must treat it exactly like no identity at all rather
// than record a profile that no later start could verify.
struct WorkerVolumeIdentity final {
    std::string disk_guid;
    std::string partition_guid;
    std::string volume_guid;

    [[nodiscard]] bool complete() const noexcept {
        return !disk_guid.empty() && !partition_guid.empty()
            && !volume_guid.empty();
    }
    bool operator==(const WorkerVolumeIdentity&) const noexcept = default;
};

// What the worker observed about the bundle it opened.
//
// The service records this as its `BundleRegistrationEvidence`, and it can come
// from nowhere else: `myrd_management` is forbidden by the execution boundary
// from opening a bundle, and the worker opens it anyway -- in `start()` it
// already re-reads the image uuid to check the identity the service claimed.
// The geometry and the uuid come free with that inspection; only the file
// identity needs a second, read-attribute handle to the bundle directory.
//
// The manifest name and the payload block size are *not* here: they are product
// constants the service already holds, and a second copy on the wire would be a
// second thing to keep in step.
struct WorkerBundleEvidence final {
    persistence::ImageUuid image_id{};
    std::uint64_t virtual_bytes{};
    std::uint32_t logical_sector_size{};
    std::uint32_t physical_sector_size{};
    // The identity of the bundle directory itself, so the service can record
    // which file object it registered rather than only which path.
    std::uint64_t volume_serial{};
    std::array<std::byte, 16> file_id{};

    // A partial identity is not a weaker one: a profile recorded from it could
    // never be re-verified against the same bundle, so every caller treats an
    // incomplete triple exactly like an absent one.
    [[nodiscard]] bool complete() const noexcept {
        return virtual_bytes != 0 && logical_sector_size != 0
            && physical_sector_size != 0 && volume_serial != 0;
    }
    bool operator==(const WorkerBundleEvidence&) const noexcept = default;
};

// The worker's own view of what it is running.  Mirrors the service-side
// WorkerRuntimeObservation, but is defined here so `src/host` never has to
// include `src/management`: the service adapter maps one onto the other.
// The two facts a forget needs and a running worker cannot supply: it holds both
// the writer authority and the proxy device by design, so "absent" can only be
// observed by something that opened the bundle without starting a runtime.
struct WorkerForgetProbe final {
    bool writer_lock_absent{};
    bool matching_device_absent{};

    bool operator==(const WorkerForgetProbe&) const noexcept = default;
};

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
    // The volume the worker adopted, which is what the service records into the
    // durable profile and what every later start checks the runtime against.
    // Absent when the worker holds no volume, or holds one whose GPT identities
    // it could not read in full -- the two are the same answer to the service,
    // and neither may be recorded as an identity.
    //
    // Declared last so every existing aggregate initializer keeps its meaning.
    std::optional<WorkerVolumeIdentity> volume;
    // The bundle this worker opened and verified, which is what the service
    // records as its registration evidence.  Absent when the worker holds no
    // bundle yet, or holds one whose identity it could not read in full -- the
    // two are the same answer to the service, and neither may be recorded.
    std::optional<WorkerBundleEvidence> bundle;
    // Whether this runtime currently holds the volume's exclusive write freeze.
    //
    // It is its own field rather than something a caller infers, because the two
    // ways a stop can be refused look identical from outside: "the runtime is
    // dirty and the writes must not be dropped" and "the write freeze was never
    // taken, so the removal has no clean moment to protect".  The service acts on
    // the difference -- the first is resolved against durable truth, the second
    // is retried -- and a status that could not tell it apart would leave the
    // caller guessing at the one moment guessing is most expensive.
    //
    // Declared last so every existing aggregate initializer keeps its meaning.
    bool writes_frozen{};
    // What only a read-only inspection can answer: whether the bundle's writer
    // authority and its proxy device are absent.
    //
    // It lives on the status because the inspection is the one command that opens
    // a bundle *without* starting the runtime that would itself hold the lock and
    // the device, and because an inspection session is dropped immediately -- so
    // there is no second command in which to ask.  Present means "a worker
    // probed"; absent means "nobody probed", which a caller must never read as
    // "both facts are false".
    std::optional<WorkerForgetProbe> forget_probe;
    // **Memory visibility (M5 §7).**  The RAM the runtime's store currently holds,
    // and the budget it was opened with.  Both are read from the storage layer's own
    // gauges inside the worker -- the process that actually owns the memory -- so the
    // service never has to infer them from a process' working set, which counts a
    // great deal that is not this disk.
    //
    // Appended with defaults so every existing aggregate initializer keeps its
    // meaning.  Zero is reported honestly as zero: a runtime that holds nothing
    // reports nothing, and a stopped runtime reports the budget it *was* given.
    std::uint64_t resident_bytes{};
    std::uint64_t budget_bytes{};
    // The bytes this runtime holds that have not reached the journal yet.  The
    // checkpoint policy's dirty threshold is expressed in bytes, so a status that
    // carried only `dirty` (a bool) and `dirty_epoch` (a per-process counter) could
    // not answer the question the policy asks.
    std::uint64_t dirty_bytes{};

    bool operator==(const WorkerRuntimeStatus&) const noexcept = default;
};

// The commit reply.
//
// It carries the post-commit observation *and* the disposition, because they are
// different facts: the status says where the runtime now stands, and the
// disposition says what the publish actually did.  Deriving the disposition from
// the observed generation would be guesswork at exactly the moment guessing is
// most expensive -- `committed_needs_reconciliation` and `commit_indeterminate`
// leave the runtime looking identical afterwards.
struct WorkerCheckpointCommitReply final {
    WorkerRuntimeStatus status;
    WorkerCommitDisposition disposition{
        WorkerCommitDisposition::not_committed};
    std::uint64_t generation{};
    // Present exactly when the commit did not cleanly land.  A landed commit
    // that also reported failure evidence would be self-contradictory and is
    // refused by `validate_shape`.
    std::optional<WorkerCommitFailure> failure;

    [[nodiscard]] bool landed() const noexcept {
        return disposition == WorkerCommitDisposition::committed
            || disposition
                == WorkerCommitDisposition::committed_needs_reconciliation;
    }
    bool operator==(const WorkerCheckpointCommitReply&) const noexcept = default;
};

// Why an abort did not succeed.
//
// The historical reply carried a persistence failure context here, and this one
// deliberately does not -- because none exists to carry. `M3Provider::
// abort_checkpoint` and the transaction abort beneath it both return
// `Result<void>`, so there is no context to relay, and composing one from the
// bare `Error` would name a persistence failure that never happened.
//
// What the caller actually needs is the distinction, not the detail: `aborted
// == false` is two facts -- there was no candidate to cancel, which leaves the
// bundle untouched, or the candidate existed and could not be cancelled, which
// leaves its state unknown and forces a reconciliation.  The safe handling for
// the first is the unsafe handling for the second, so the reason travels as a
// value of its own until the layer below can supply a real context.
enum class WorkerAbortRefusal : std::uint8_t {
    none = 0,
    no_candidate,
    abort_failed,
};

struct WorkerCheckpointAbortReply final {
    bool aborted{};
    // The generation that remains committed.  A successful abort does not move
    // it, which is what lets a caller record that the candidate was not
    // published; a caller that could not read it could not make that check.
    std::uint64_t generation{};
    WorkerAbortRefusal refusal{WorkerAbortRefusal::none};

    [[nodiscard]] bool refused() const noexcept {
        return refusal != WorkerAbortRefusal::none;
    }
    bool operator==(const WorkerCheckpointAbortReply&) const noexcept = default;
};

// Why a removal did not happen.  `none` means it was not refused, and the
// protocol requires that to agree with `removed`/`stopped` in both directions.
//
// A refusal is a first-class answer rather than an error, because it is a
// decision the worker reached about its *own* runtime and the reason is the only
// part the caller can act on.  `runtime_dirty` says the worker holds uncommitted
// writes and that removal would drop them -- which the caller resolves against
// durable truth rather than overriding.  Reported as a generic failure it would
// be indistinguishable from a broken channel, and a caller that cannot tell
// "the worker refused" from "the worker did not answer" will retry the one thing
// it must not retry.
enum class WorkerRemovalRefusal : std::uint8_t {
    none = 0,
    runtime_dirty,
};

// The removal answer.
//
// `generation` is the generation the worker stood at when it removed, and the
// caller compares it against the one its durable compare-and-swap authorized: a
// removal proven at any other generation is not a removal the caller may record.
// `removed` and `stopped` are separate because the barrier is supposed to do
// both in one authenticated call, so a reply claiming one without the other
// describes a partial teardown as a complete one -- and the caller records a
// removal either way.
struct WorkerRemoveAndStopReply final {
    bool removed{};
    bool stopped{};
    std::uint64_t generation{};
    WorkerRemovalRefusal refusal{WorkerRemovalRefusal::none};

    [[nodiscard]] bool refused() const noexcept {
        return refusal != WorkerRemovalRefusal::none;
    }
    bool operator==(const WorkerRemoveAndStopReply&) const noexcept = default;
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
    // The commit reply.  Distinct from `status` on purpose: a commit answers with
    // its disposition *and* an observation, and folding them into `status` is
    // what lost the disposition in the first place.
    std::optional<WorkerCheckpointCommitReply> commit_reply;
    // The removal answer, its own type for the same reason: a caller has to
    // learn which generation the removal happened at, and why it did not happen
    // when it did not.  An observation carries neither.
    std::optional<WorkerRemoveAndStopReply> removal_reply;
    // The abort answer, its own type for the same reason: `aborted == false`
    // alone does not say whether there was anything to cancel.
    std::optional<WorkerCheckpointAbortReply> abort_reply;
    std::optional<WorkerRuntimeStatus> status;
    std::optional<WorkerRecoveryProof> recovery;
    // Carried by a recovery-proof request: what the service durably recorded at
    // P4, which is the only thing a fresh worker may judge ownership against.
    std::optional<WorkerRecoveryCorrelation> correlation;
    // Carried by a remove-and-stop request: the service's ownership proof.
    // Without it the worker has nothing to remove *as its own* and must refuse.
    std::optional<WorkerRemovalAuthorization> removal;
    // Carried by an export request: the absolute file the worker must create.
    // UTF-16 because it is a Windows path, and the message is already a private
    // in-process channel with a fixed 64 KiB bound.
    std::optional<std::wstring> export_path;
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
