#include "src/host/m5_worker_protocol.h"

#include "src/host/m5_worker_bytes.h"

#include <array>
#include <cstring>
#include <utility>

namespace myrd::m5 {
namespace {

// "M5WK" on the wire.  A distinct magic keeps a public-pipe frame from ever
// being decoded as a private worker message.
constexpr std::uint32_t kWorkerMagic = 0x4B57354DU;
constexpr std::size_t kFixedHeaderBytes = 4 + 2 + 2 + 2 + 2 + 8 + 4;

// Presence bits, in wire order.
constexpr std::uint32_t kHasBinding = 1U << 0;
constexpr std::uint32_t kHasOperationId = 1U << 1;
constexpr std::uint32_t kHasHandshake = 1U << 2;
constexpr std::uint32_t kHasPrepared = 1U << 3;
constexpr std::uint32_t kHasCommit = 1U << 4;
constexpr std::uint32_t kHasStatus = 1U << 5;
constexpr std::uint32_t kHasRecovery = 1U << 6;
constexpr std::uint32_t kHasError = 1U << 7;
constexpr std::uint32_t kHasCorrelation = 1U << 8;
constexpr std::uint32_t kHasRemoval = 1U << 9;
constexpr std::uint32_t kHasCommitReply = 1U << 10;

Error protocol_error(const std::uint32_t code, const wchar_t* operation,
                     const wchar_t* message) {
    return {ErrorDomain::protocol, code, operation, message, 0};
}

template <typename T>
Result<T> malformed(const wchar_t* operation, const wchar_t* message) {
    return Result<T>::failure(protocol_error(
        static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
        operation, message));
}

std::uint32_t presence_mask(const WorkerMessage& message) {
    std::uint32_t mask = 0;
    if (message.binding.has_value()) mask |= kHasBinding;
    if (message.product_operation_id.has_value()) mask |= kHasOperationId;
    if (message.handshake.has_value()) mask |= kHasHandshake;
    if (message.prepared.has_value()) mask |= kHasPrepared;
    if (message.commit.has_value()) mask |= kHasCommit;
    if (message.status.has_value()) mask |= kHasStatus;
    if (message.commit_reply.has_value()) mask |= kHasCommitReply;
    if (message.recovery.has_value()) mask |= kHasRecovery;
    if (message.error.has_value()) mask |= kHasError;
    if (message.correlation.has_value()) mask |= kHasCorrelation;
    if (message.removal.has_value()) mask |= kHasRemoval;
    return mask;
}

// The payload a command must carry, per kind.  A message that carries anything
// else is refused: an unexpected field is how a digest would stop being checked.
struct PayloadShape final {
    std::uint32_t required{};
    std::uint32_t optional{};
};

PayloadShape shape_for(const WorkerMessageKind kind, const WorkerCommand command) {
    // Every message is addressed to one bound worker, so the binding is
    // required for all of them -- requests and replies alike.
    constexpr std::uint32_t kBase = kHasBinding;
    switch (command) {
    case WorkerCommand::handshake:
        return {kBase | kHasHandshake, kHasError};
    case WorkerCommand::start:
    case WorkerCommand::checkpoint_abort:
        // Operation-scoped: the shape requires an operation id, so the gate can
        // require one for exactly the commands that must be attributable.
        return {kBase | kHasOperationId |
                    (kind == WorkerMessageKind::reply ? kHasStatus : 0U),
                kHasError};
    case WorkerCommand::status:
    case WorkerCommand::require_runtime_observation:
        // *Not* operation-scoped.  These ask what a worker is doing on behalf of
        // the service rather than of one operation, so an operation id is neither
        // required nor defined -- a stray one is refused like any other stray
        // field.  The payload belongs on the reply, because these *ask* for it.
        return {kBase | (kind == WorkerMessageKind::reply ? kHasStatus : 0U),
                kHasError};
    case WorkerCommand::shutdown:
        // Also not operation-scoped.  The reply carries a status only when there
        // is one to report: a worker that was never started is still a worker
        // that was told to shut down.
        return {kBase, kHasStatus | kHasError};
    case WorkerCommand::cleanup:
        return {kBase | (kind == WorkerMessageKind::reply ? kHasStatus : 0U),
                kHasOperationId | kHasError};
    case WorkerCommand::checkpoint_prepare:
        return {kBase | kHasOperationId |
                    (kind == WorkerMessageKind::reply ? kHasPrepared : 0U),
                kHasError};
    case WorkerCommand::checkpoint_commit:
        // The reply carries the commit reply payload, not a bare status: a
        // commit has to report what the publish *did*, and an observation alone
        // cannot say whether the bytes landed.
        return {kBase | kHasOperationId |
                    (kind == WorkerMessageKind::request ? kHasCommit
                                                        : kHasCommitReply),
                kHasError};
    case WorkerCommand::recovery_proof:
        // A recovery proof request must carry the P4 correlation to judge
        // against; without it there is nothing to prove ownership of.
        return {kBase | kHasOperationId |
                    (kind == WorkerMessageKind::reply ? kHasRecovery
                                                      : kHasCorrelation),
                kHasError};
    case WorkerCommand::remove_and_stop:
        // The request must carry the ownership authorization; there is no shape
        // in which a worker is asked to remove something without being told
        // what it has proved it owns.
        return {kBase | kHasOperationId |
                    (kind == WorkerMessageKind::request ? kHasRemoval
                                                        : kHasStatus),
                kHasError};
    }
    return {};
}

Result<void> validate_shape(const WorkerMessage& message) {
    const auto shape = shape_for(message.kind, message.command);
    const auto mask = presence_mask(message);
    const auto stray = mask & ~(shape.required | shape.optional);
    if (stray != 0) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"decode_worker_message",
            L"message carries a payload its command does not define"));
    }
    // A reply reports either a success payload or an error, never both.  An
    // error reply therefore satisfies the shape without its success payload --
    // but carrying both would let a caller read a stale payload past a reported
    // failure, so that combination is refused.
    //
    // The binding and the operation id are *envelope*, not payload: every
    // message is addressed to a bound worker and, where the command is
    // operation-scoped, to one operation.  An error reply carries both, and
    // treating the operation id as a payload would make every error reply to an
    // operation-scoped command unbuildable.
    constexpr std::uint32_t kEnvelope = kHasBinding | kHasOperationId;
    if (message.error.has_value()) {
        if ((mask & shape.required & ~kEnvelope) != 0) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"error reply must not also carry a success payload"));
        }
        return Result<void>::success();
    }
    if ((mask & shape.required) != shape.required) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"decode_worker_message",
            L"message is missing a payload its command requires"));
    }
    // A status carrying part of a volume identity is self-inconsistent in the
    // same way a truncated body is, so it is refused here, where both the
    // encoder and the decoder pass, rather than at either end separately.  A
    // partial identity is not a weaker proof: it is one the service cannot
    // compare against a durable profile at all, and letting it through would let
    // a profile be recorded that no later start could ever verify.
    if (message.status.has_value() && message.status->volume.has_value()
        && !message.status->volume->complete()) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"validate_shape",
            L"The status carries an incomplete volume identity"));
    }
    // A commit reply says either "it landed" or "here is why it did not", never
    // both.  The two halves are what ServiceCore validates its own evidence
    // against, so a reply carrying both would let a caller read a failure past a
    // landed commit -- or, worse, read a landed commit past the failure that
    // actually happened.
    if (message.commit_reply.has_value()) {
        if (message.commit_reply->landed()
            && message.commit_reply->failure.has_value()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"validate_shape",
                L"A landed commit must not also carry failure evidence"));
        }
        if (!message.commit_reply->landed()
            && !message.commit_reply->failure.has_value()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"validate_shape",
                L"A commit that did not land must carry the failure that "
                L"decided it"));
        }
    }
    return Result<void>::success();
}

// The one status decoder, mirroring `write_status` above.  Its truncation checks
// stay here rather than being repeated at the two call sites.
Result<WorkerRuntimeStatus> read_status(detail::WorkerBytesReader& reader) {
    WorkerRuntimeStatus status{};
    status.worker_alive = reader.boolean();
    status.image_id = reader.image_uuid();
    status.committed_generation = reader.u64();
    status.dirty = reader.boolean();
    status.dirty_epoch = reader.u64();
    if (reader.boolean()) {
        auto active = reader.uuid();
        if (!active.has_value()) {
            return malformed<WorkerRuntimeStatus>(
                L"decode_worker_message",
                L"active operation id is not a valid uuid");
        }
        status.active_operation_id = *active;
    }
    status.active_operation_phase = reader.u32();
    // Presence, then the three texts.  A truncated identity is caught here; a
    // complete but partial triple is caught by `validate_shape`, which both the
    // encoder and the decoder pass through.
    if (reader.boolean()) {
        WorkerVolumeIdentity volume{};
        volume.disk_guid = reader.text_ascii();
        volume.partition_guid = reader.text_ascii();
        volume.volume_guid = reader.text_ascii();
        if (!reader.ok()) {
            return malformed<WorkerRuntimeStatus>(
                L"decode_worker_message",
                L"status volume identity is truncated");
        }
        status.volume = std::move(volume);
    }
    return Result<WorkerRuntimeStatus>::success(std::move(status));
}

Result<void> parse_body(detail::WorkerBytesReader& reader, const std::uint32_t mask,
                        WorkerMessage& message) {
    if ((mask & kHasBinding) != 0) {
        const std::uint32_t process_id = reader.u32();
        const std::uint64_t process_creation_time = reader.u64();
        const std::uint64_t session_generation = reader.u64();
        auto profile_id = reader.uuid();
        if (!profile_id.has_value()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"binding profile id is not a valid uuid"));
        }
        message.binding = WorkerBinding{process_id, process_creation_time,
                                        session_generation, *profile_id};
    }
    if ((mask & kHasOperationId) != 0) {
        auto operation_id = reader.uuid();
        if (!operation_id.has_value()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"product operation id is not a valid uuid"));
        }
        message.product_operation_id = *operation_id;
    }
    if ((mask & kHasHandshake) != 0) {
        WorkerHandshake handshake{};
        handshake.protocol_major = reader.u16();
        handshake.protocol_minor = reader.u16();
        handshake.session_nonce = reader.u64();
        const auto secret = reader.raw(handshake.startup_secret.bytes.size());
        if (!reader.ok()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"handshake startup secret is truncated"));
        }
        std::memcpy(handshake.startup_secret.bytes.data(), secret.data(),
                    handshake.startup_secret.bytes.size());
        message.handshake = handshake;
    }
    if ((mask & kHasPrepared) != 0) {
        WorkerPreparedCandidate prepared{};
        prepared.prior_generation = reader.u64();
        prepared.candidate_generation = reader.u64();
        prepared.native_operation_id = reader.u64();
        prepared.cut_dirty_epoch = reader.u64();
        prepared.manifest_slot = reader.u8();
        prepared.candidate_bytes_digest = reader.digest();
        message.prepared = prepared;
    }
    if ((mask & kHasCommit) != 0) {
        WorkerCheckpointCommit commit{};
        commit.native_operation_id = reader.u64();
        commit.manifest_slot = reader.u8();
        commit.candidate_bytes_digest = reader.digest();
        message.commit = commit;
    }
    // Read in wire order, which is the order `write_body` writes: the commit
    // reply precedes the bare status.  The reader is a cursor, so a block read
    // out of order would not fail -- it would decode the wrong bytes into the
    // right fields.
    if ((mask & kHasCommitReply) != 0) {
        WorkerCheckpointCommitReply reply{};
        auto status = read_status(reader);
        if (!status.ok()) return Result<void>::failure(status.error());
        reply.status = status.move_value();
        const auto disposition = reader.u8();
        if (disposition
            > static_cast<std::uint8_t>(
                WorkerCommitDisposition::committed_needs_reconciliation)) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"commit disposition is not a value this protocol defines"));
        }
        reply.disposition = static_cast<WorkerCommitDisposition>(disposition);
        reply.generation = reader.u64();
        if (reader.boolean()) {
            WorkerCommitFailure failure{};
            failure.operation = reader.text_ascii();
            failure.code = reader.u32();
            failure.phase = reader.u8();
            failure.native_code = reader.u32();
            failure.native_operation_id = reader.u64();
            failure.image_uuid = reader.image_uuid();
            failure.generation = reader.u64();
            failure.safe_context = reader.text_ascii();
            if (!reader.ok()) {
                return Result<void>::failure(protocol_error(
                    static_cast<std::uint32_t>(
                        OperationErrorCode::protocol_malformed),
                    L"decode_worker_message",
                    L"commit failure evidence is truncated"));
            }
            reply.failure = std::move(failure);
        }
        if (!reader.ok()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"commit reply is truncated"));
        }
        message.commit_reply = std::move(reply);
    }
    if ((mask & kHasStatus) != 0) {
        auto status = read_status(reader);
        if (!status.ok()) return Result<void>::failure(status.error());
        message.status = status.move_value();
    }
    if ((mask & kHasRecovery) != 0) {
        WorkerRecoveryProof recovery{};
        recovery.outcome = static_cast<WorkerRecoveryOutcome>(reader.u8());
        recovery.observed_generation = reader.u64();
        recovery.native_operation_id = reader.u64();
        recovery.manifest_slot = reader.u8();
        recovery.candidate_bytes_digest = reader.digest();
        message.recovery = recovery;
    }
    if ((mask & kHasCorrelation) != 0) {
        // Read in wire order; the product id has no default constructor, so it
        // is parsed before the aggregate is built.
        auto product_operation_id = reader.uuid();
        if (!product_operation_id.has_value()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"recovery correlation product operation id is not a valid uuid"));
        }
        persistence::CheckpointRecoveryCorrelation checkpoint{};
        checkpoint.image_uuid = reader.image_uuid();
        checkpoint.prior_generation = reader.u64();
        checkpoint.candidate_generation = reader.u64();
        checkpoint.native_operation_id = reader.u64();
        checkpoint.manifest_slot = reader.u8();
        checkpoint.candidate_bytes_digest = reader.digest();
        if (!reader.ok()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"recovery correlation body is truncated"));
        }
        message.correlation = WorkerRecoveryCorrelation{
            *product_operation_id, checkpoint};
    }
    if ((mask & kHasRemoval) != 0) {
        auto removal_operation_id = reader.uuid();
        auto profile_id = reader.uuid();
        if (!removal_operation_id.has_value() || !profile_id.has_value()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"removal authorization ids are not valid uuids"));
        }
        WorkerRemovalAuthorization authorization{*removal_operation_id,
                                                 *profile_id};
        authorization.image_id = reader.image_uuid();
        authorization.ownership_evidence_digest = reader.digest();
        // Read as explicit booleans rather than a packed flag word: a decoder
        // that dropped a bit would silently turn one missing proof into a
        // present one.
        authorization.writer_lock_proved = reader.boolean();
        authorization.device_match_proved = reader.boolean();
        authorization.bundle_identity_proved = reader.boolean();
        if (!reader.ok()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"removal authorization body is truncated"));
        }
        message.removal = authorization;
    }
    if ((mask & kHasError) != 0) {
        Error error{};
        error.domain = static_cast<ErrorDomain>(reader.u8());
        error.code = reader.u32();
        error.operation = reader.text();
        error.message = reader.text();
        error.native_code = reader.u32();
        message.error = std::move(error);
    }
    if (!reader.ok()) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::protocol_malformed),
            L"decode_worker_message", L"message body is truncated"));
    }
    return Result<void>::success();
}

// One status encoding, used by both the bare `status` payload and the commit
// reply that carries a status alongside its disposition.  Two copies would be
// two places for the field order to drift, and a drifted field order does not
// fail to compile -- it decodes as a plausible status with the wrong numbers in
// it, which is the worst available failure mode for a channel whose whole
// purpose is to be trustworthy.
void write_status(detail::WorkerBytesWriter& writer,
                  const WorkerRuntimeStatus& status) {
    writer.u8(status.worker_alive ? 1 : 0);
    writer.image_uuid(status.image_id);
    writer.u64(status.committed_generation);
    writer.u8(status.dirty ? 1 : 0);
    writer.u64(status.dirty_epoch);
    writer.u8(status.active_operation_id.has_value() ? 1 : 0);
    if (status.active_operation_id.has_value())
        writer.uuid(*status.active_operation_id);
    writer.u32(status.active_operation_phase);
    // Present only when all three identities were read; `validate_shape`
    // refuses a partial triple before this is reached, so what is written here
    // is either a complete identity or none.
    writer.boolean(status.volume.has_value());
    if (status.volume.has_value()) {
        writer.text_ascii(status.volume->disk_guid);
        writer.text_ascii(status.volume->partition_guid);
        writer.text_ascii(status.volume->volume_guid);
    }
}

void write_body(detail::WorkerBytesWriter& writer, const WorkerMessage& message) {
    if (message.binding.has_value()) {
        writer.u32(message.binding->process_id);
        writer.u64(message.binding->process_creation_time);
        writer.u64(message.binding->session_generation);
        writer.uuid(message.binding->profile_id);
    }
    if (message.product_operation_id.has_value())
        writer.uuid(*message.product_operation_id);
    if (message.handshake.has_value()) {
        writer.u16(message.handshake->protocol_major);
        writer.u16(message.handshake->protocol_minor);
        writer.u64(message.handshake->session_nonce);
        writer.raw(message.handshake->startup_secret.bytes);
    }
    if (message.prepared.has_value()) {
        writer.u64(message.prepared->prior_generation);
        writer.u64(message.prepared->candidate_generation);
        writer.u64(message.prepared->native_operation_id);
        writer.u64(message.prepared->cut_dirty_epoch);
        writer.u8(message.prepared->manifest_slot);
        writer.digest(message.prepared->candidate_bytes_digest);
    }
    if (message.commit.has_value()) {
        writer.u64(message.commit->native_operation_id);
        writer.u8(message.commit->manifest_slot);
        writer.digest(message.commit->candidate_bytes_digest);
    }
    if (message.commit_reply.has_value()) {
        write_status(writer, message.commit_reply->status);
        writer.u8(static_cast<std::uint8_t>(message.commit_reply->disposition));
        writer.u64(message.commit_reply->generation);
        // Present exactly when the commit did not cleanly land; `validate_shape`
        // refuses the contradictory combination before this is reached.
        writer.boolean(message.commit_reply->failure.has_value());
        if (message.commit_reply->failure.has_value()) {
            const auto& failure = *message.commit_reply->failure;
            writer.text_ascii(failure.operation);
            writer.u32(failure.code);
            writer.u8(failure.phase);
            writer.u32(failure.native_code);
            writer.u64(failure.native_operation_id);
            writer.image_uuid(failure.image_uuid);
            writer.u64(failure.generation);
            writer.text_ascii(failure.safe_context);
        }
    }
    if (message.status.has_value()) write_status(writer, *message.status);
    if (message.recovery.has_value()) {
        writer.u8(static_cast<std::uint8_t>(message.recovery->outcome));
        writer.u64(message.recovery->observed_generation);
        writer.u64(message.recovery->native_operation_id);
        writer.u8(message.recovery->manifest_slot);
        writer.digest(message.recovery->candidate_bytes_digest);
    }
    if (message.correlation.has_value()) {
        writer.uuid(message.correlation->product_operation_id);
        writer.image_uuid(message.correlation->checkpoint.image_uuid);
        writer.u64(message.correlation->checkpoint.prior_generation);
        writer.u64(message.correlation->checkpoint.candidate_generation);
        writer.u64(message.correlation->checkpoint.native_operation_id);
        writer.u8(message.correlation->checkpoint.manifest_slot);
        writer.digest(message.correlation->checkpoint.candidate_bytes_digest);
    }
    if (message.removal.has_value()) {
        writer.uuid(message.removal->removal_operation_id);
        writer.uuid(message.removal->profile_id);
        writer.image_uuid(message.removal->image_id);
        writer.digest(message.removal->ownership_evidence_digest);
        writer.u8(message.removal->writer_lock_proved ? 1 : 0);
        writer.u8(message.removal->device_match_proved ? 1 : 0);
        writer.u8(message.removal->bundle_identity_proved ? 1 : 0);
    }
    if (message.error.has_value()) {
        writer.u8(static_cast<std::uint8_t>(message.error->domain));
        writer.u32(message.error->code);
        writer.text(message.error->operation);
        writer.text(message.error->message);
        writer.u32(message.error->native_code);
    }
}

} // namespace

Result<std::vector<std::byte>> encode_worker_message(
    const WorkerMessage& message) {
    auto shape = validate_shape(message);
    if (!shape.ok()) return Result<std::vector<std::byte>>::failure(shape.error());

    detail::WorkerBytesWriter writer;
    writer.u32(kWorkerMagic);
    writer.u16(kWorkerProtocolMajor);
    writer.u16(kWorkerProtocolMinor);
    writer.u16(static_cast<std::uint16_t>(message.kind));
    writer.u16(static_cast<std::uint16_t>(message.command));
    writer.u64(message.sequence);
    writer.u32(presence_mask(message));
    write_body(writer, message);

    auto bytes = writer.take();
    if (bytes.size() > kMaxWorkerMessageBytes) {
        return Result<std::vector<std::byte>>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::invalid_range),
            L"encode_worker_message",
            L"message exceeds the maximum private-protocol size"));
    }
    return Result<std::vector<std::byte>>::success(std::move(bytes));
}

Result<WorkerMessage> decode_worker_message(
    const std::span<const std::byte> bytes) {
    if (bytes.size() > kMaxWorkerMessageBytes)
        return malformed<WorkerMessage>(
            L"decode_worker_message", L"message exceeds the maximum size");
    if (bytes.size() < kFixedHeaderBytes)
        return malformed<WorkerMessage>(
            L"decode_worker_message", L"message is shorter than its header");

    detail::WorkerBytesReader reader(bytes);
    if (reader.u32() != kWorkerMagic)
        return malformed<WorkerMessage>(
            L"decode_worker_message", L"message magic is not the private protocol");
    const auto major = reader.u16();
    const auto minor = reader.u16();
    if (major != kWorkerProtocolMajor || minor != kWorkerProtocolMinor) {
        return malformed<WorkerMessage>(
            L"decode_worker_message", L"unsupported private protocol version");
    }

    WorkerMessage message{};
    message.kind = static_cast<WorkerMessageKind>(reader.u16());
    message.command = static_cast<WorkerCommand>(reader.u16());
    message.sequence = reader.u64();
    const auto mask = reader.u32();
    if (!reader.ok())
        return malformed<WorkerMessage>(
            L"decode_worker_message", L"message header is truncated");

    if ((mask & kHasBinding) == 0)
        return malformed<WorkerMessage>(
            L"decode_worker_message", L"message does not carry a worker binding");

    auto body = parse_body(reader, mask, message);
    if (!body.ok()) return Result<WorkerMessage>::failure(body.error());

    // Trailing bytes mean the sender and this decoder disagree about the
    // message.  Accepting them would let a field be silently dropped.
    if (!reader.exhausted())
        return malformed<WorkerMessage>(
            L"decode_worker_message", L"message has trailing bytes");

    auto shape = validate_shape(message);
    if (!shape.ok()) return Result<WorkerMessage>::failure(shape.error());
    return Result<WorkerMessage>::success(std::move(message));
}

Result<void> authenticate_worker_message(
    const WorkerMessage& message, const WorkerBinding& expected_binding,
    const std::uint64_t last_accepted_sequence) {
    if (!message.binding.has_value()) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"authenticate_worker_message",
            L"message does not carry an authenticated binding"));
    }
    if (!(*message.binding == expected_binding)) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"authenticate_worker_message",
            L"message binding does not match this session"));
    }
    // Strictly greater, not merely different: a lower or repeated sequence is a
    // replay of an earlier command and must not run again.
    if (message.sequence <= last_accepted_sequence) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"authenticate_worker_message",
            L"message sequence was already accepted"));
    }
    // The operation id is required exactly where the command's own shape says
    // it is required, so "operation-scoped" is stated once rather than twice.
    // An earlier revision required it of every command but the handshake, which
    // made a status query -- which belongs to no operation -- unanswerable.
    if ((shape_for(message.kind, message.command).required & kHasOperationId) !=
            0 &&
        !message.product_operation_id.has_value()) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"authenticate_worker_message",
            L"message does not carry the product operation id its command needs"));
    }
    if (message.command == WorkerCommand::handshake) {
        // Guarded rather than assumed: a message may reach here undecoded (a
        // test or an in-process caller), so the payload is checked, not trusted.
        if (!message.handshake.has_value()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
                L"authenticate_worker_message",
                L"handshake carries no handshake payload"));
        }
        if (message.handshake->protocol_major != kWorkerProtocolMajor) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
                L"authenticate_worker_message",
                L"handshake protocol major does not match"));
        }
    }
    return Result<void>::success();
}

Result<void> require_matching_candidate(
    const WorkerCheckpointCommit& commit,
    const WorkerPreparedCandidate& prepared) {
    if (commit.native_operation_id != prepared.native_operation_id) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"require_matching_candidate",
            L"commit native operation id is not the prepared candidate"));
    }
    if (commit.manifest_slot != prepared.manifest_slot) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"require_matching_candidate",
            L"commit manifest slot is not the prepared candidate"));
    }
    if (commit.candidate_bytes_digest != prepared.candidate_bytes_digest) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"require_matching_candidate",
            L"commit digest is not the prepared candidate"));
    }
    return Result<void>::success();
}

Result<void> require_removal_authorized(
    const WorkerRemovalAuthorization& authorization,
    const WorkerBinding& binding,
    const persistence::ImageUuid& observed_image_id) {
    // Each proof is checked separately and named in the refusal, so a caller
    // recording the failure records *which* proof was missing rather than a
    // generic "unsafe".
    if (!authorization.writer_lock_proved) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"require_removal_authorized",
            L"removal is refused: the bundle writer lock was not proved"));
    }
    if (!authorization.device_match_proved) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"require_removal_authorized",
            L"removal is refused: the matching device was not proved"));
    }
    if (!authorization.bundle_identity_proved) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"require_removal_authorized",
            L"removal is refused: the retained bundle identity was not proved"));
    }
    if (authorization.profile_id != binding.profile_id) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"require_removal_authorized",
            L"removal is refused: the authorization names another profile"));
    }
    // The proof is about a bundle, so it must name the bundle this worker is
    // actually bound to.  A complete set of proofs for a *different* image is
    // not a proof for this one.
    if (!(authorization.image_id == observed_image_id)) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"require_removal_authorized",
            L"removal is refused: the authorization names another image"));
    }
    return Result<void>::success();
}

WorkerRecoveryOutcome worker_recovery_outcome(
    const persistence::CheckpointRecoveryVerdict verdict) noexcept {
    switch (verdict) {
    case persistence::CheckpointRecoveryVerdict::CandidateGenerationProved:
        return WorkerRecoveryOutcome::candidate_generation_proved;
    case persistence::CheckpointRecoveryVerdict::PriorGenerationProved:
        return WorkerRecoveryOutcome::prior_generation_proved;
    case persistence::CheckpointRecoveryVerdict::OwnershipUnprovable:
        return WorkerRecoveryOutcome::ownership_unprovable;
    }
    // Unreachable for the three declared cases; an added persistence verdict
    // falls here, and the fail-closed answer is that nothing was proved.
    return WorkerRecoveryOutcome::ownership_unprovable;
}

} // namespace myrd::m5
