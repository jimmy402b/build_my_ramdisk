#include "src/host/m5_worker_protocol.h"

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

class Writer final {
public:
    void u8(const std::uint8_t value) {
        bytes_.push_back(static_cast<std::byte>(value));
    }
    void u16(const std::uint16_t value) {
        for (int shift = 0; shift < 16; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void u32(const std::uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void u64(const std::uint64_t value) {
        for (int shift = 0; shift < 64; shift += 8)
            u8(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
    }
    void raw(std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void uuid(const product::Uuid& value) { raw(value.bytes()); }
    void digest(const Sha256Digest& value) {
        raw(std::span<const std::byte>(value.data(), value.size()));
    }
    // Stored as UTF-16LE code units with a code-unit count so the decoder never
    // has to trust a byte length to describe a character count.
    void text(const std::wstring& value) {
        u16(static_cast<std::uint16_t>(value.size()));
        for (const wchar_t unit : value) {
            const auto code_unit = static_cast<std::uint16_t>(unit);
            u8(static_cast<std::uint8_t>(code_unit & 0xFFU));
            u8(static_cast<std::uint8_t>((code_unit >> 8) & 0xFFU));
        }
    }

    std::vector<std::byte> take() { return std::move(bytes_); }

private:
    std::vector<std::byte> bytes_;
};

class Reader final {
public:
    explicit Reader(const std::span<const std::byte> bytes) : bytes_(bytes) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] bool exhausted() const noexcept { return offset_ == bytes_.size(); }

    std::uint8_t u8() {
        if (!ok_ || offset_ + 1 > bytes_.size()) return fail();
        return std::to_integer<std::uint8_t>(bytes_[offset_++]);
    }
    std::uint16_t u16() {
        std::uint16_t value = 0;
        for (int shift = 0; shift < 16; shift += 8)
            value |= static_cast<std::uint16_t>(u8()) << shift;
        return value;
    }
    std::uint32_t u32() {
        std::uint32_t value = 0;
        for (int shift = 0; shift < 32; shift += 8)
            value |= static_cast<std::uint32_t>(u8()) << shift;
        return value;
    }
    std::uint64_t u64() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 8)
            value |= static_cast<std::uint64_t>(u8()) << shift;
        return value;
    }
    std::span<const std::byte> raw(const std::size_t count) {
        if (!ok_ || offset_ + count > bytes_.size() || offset_ + count < offset_) {
            (void)fail();
            return {};
        }
        const auto slice = bytes_.subspan(offset_, count);
        offset_ += count;
        return slice;
    }
    bool boolean() { return u8() != 0; }
    // product::Uuid has a private default constructor and only a canonical-text
    // factory, so a decoded id comes back through parse() or not at all.
    std::optional<product::Uuid> uuid() {
        const auto slice = raw(16);
        if (!ok_) return std::nullopt;
        std::string text_value;
        text_value.reserve(36);
        static constexpr char kHex[] = "0123456789abcdef";
        // Canonical 8-4-4-4-12 lowercase form, byte-identical to the helper the
        // public pipe codec already uses.
        for (std::size_t index = 0; index < slice.size(); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10)
                text_value.push_back('-');
            const auto byte = std::to_integer<std::uint8_t>(slice[index]);
            text_value.push_back(kHex[byte >> 4]);
            text_value.push_back(kHex[byte & 0x0FU]);
        }
        auto parsed = product::Uuid::parse(text_value);
        if (!parsed.ok()) {
            (void)fail();
            return std::nullopt;
        }
        return parsed.value();
    }
    Sha256Digest digest() {
        Sha256Digest value{};
        const auto slice = raw(value.size());
        if (!ok_) return value;
        std::memcpy(value.data(), slice.data(), value.size());
        return value;
    }
    std::wstring text() {
        const auto count = u16();
        std::wstring value;
        value.reserve(count);
        for (std::uint16_t index = 0; index < count && ok_; ++index) {
            const auto low = static_cast<std::uint16_t>(u8());
            const auto high = static_cast<std::uint16_t>(u8());
            value.push_back(static_cast<wchar_t>(low | (high << 8)));
        }
        return value;
    }

private:
    std::uint8_t fail() {
        ok_ = false;
        return 0;
    }

    std::span<const std::byte> bytes_;
    std::size_t offset_{};
    bool ok_{true};
};

std::uint32_t presence_mask(const WorkerMessage& message) {
    std::uint32_t mask = 0;
    if (message.binding.has_value()) mask |= kHasBinding;
    if (message.product_operation_id.has_value()) mask |= kHasOperationId;
    if (message.handshake.has_value()) mask |= kHasHandshake;
    if (message.prepared.has_value()) mask |= kHasPrepared;
    if (message.commit.has_value()) mask |= kHasCommit;
    if (message.status.has_value()) mask |= kHasStatus;
    if (message.recovery.has_value()) mask |= kHasRecovery;
    if (message.error.has_value()) mask |= kHasError;
    if (message.correlation.has_value()) mask |= kHasCorrelation;
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
    case WorkerCommand::status:
    case WorkerCommand::checkpoint_abort:
    case WorkerCommand::runtime_observation:
    case WorkerCommand::cleanup:
        return {kBase | kHasStatus, kHasOperationId | kHasError};
    case WorkerCommand::checkpoint_prepare:
        return {kBase | (kind == WorkerMessageKind::reply ? kHasPrepared : 0U),
                kHasOperationId | kHasError};
    case WorkerCommand::checkpoint_commit:
        return {kBase | (kind == WorkerMessageKind::request ? kHasCommit
                                                            : kHasStatus),
                kHasOperationId | kHasError};
    case WorkerCommand::recovery_proof:
        // A recovery proof request must carry the P4 correlation to judge
        // against; without it there is nothing to prove ownership of.
        return {kBase | (kind == WorkerMessageKind::reply ? kHasRecovery
                                                          : kHasCorrelation),
                kHasOperationId | kHasError};
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
    if (message.error.has_value()) {
        if ((mask & shape.required & ~kHasBinding) != 0) {
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
    return Result<void>::success();
}

Result<void> parse_body(Reader& reader, const std::uint32_t mask,
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
    if ((mask & kHasStatus) != 0) {
        WorkerRuntimeStatus status{};
        status.worker_alive = reader.boolean();
        status.committed_generation = reader.u64();
        status.dirty = reader.boolean();
        status.dirty_epoch = reader.u64();
        if (reader.boolean()) {
            auto active = reader.uuid();
            if (!active.has_value()) {
                return Result<void>::failure(protocol_error(
                    static_cast<std::uint32_t>(
                        OperationErrorCode::protocol_malformed),
                    L"decode_worker_message",
                    L"active operation id is not a valid uuid"));
            }
            status.active_operation_id = *active;
        }
        status.active_operation_phase = reader.u32();
        message.status = status;
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
        // Read in wire order; the two ids have no default constructor, so they
        // are parsed before the aggregate is built.
        auto product_operation_id = reader.uuid();
        auto image_id = reader.uuid();
        const std::uint64_t prior_generation = reader.u64();
        const std::uint64_t candidate_generation = reader.u64();
        const std::uint64_t native_operation_id = reader.u64();
        const std::uint8_t manifest_slot = reader.u8();
        const Sha256Digest candidate_bytes_digest = reader.digest();
        if (!product_operation_id.has_value() || !image_id.has_value()) {
            return Result<void>::failure(protocol_error(
                static_cast<std::uint32_t>(
                    OperationErrorCode::protocol_malformed),
                L"decode_worker_message",
                L"recovery correlation ids are not valid uuids"));
        }
        message.correlation = WorkerRecoveryCorrelation{
            *product_operation_id, *image_id, prior_generation,
            candidate_generation, native_operation_id, manifest_slot,
            candidate_bytes_digest};
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

void write_body(Writer& writer, const WorkerMessage& message) {
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
    if (message.status.has_value()) {
        writer.u8(message.status->worker_alive ? 1 : 0);
        writer.u64(message.status->committed_generation);
        writer.u8(message.status->dirty ? 1 : 0);
        writer.u64(message.status->dirty_epoch);
        writer.u8(message.status->active_operation_id.has_value() ? 1 : 0);
        if (message.status->active_operation_id.has_value())
            writer.uuid(*message.status->active_operation_id);
        writer.u32(message.status->active_operation_phase);
    }
    if (message.recovery.has_value()) {
        writer.u8(static_cast<std::uint8_t>(message.recovery->outcome));
        writer.u64(message.recovery->observed_generation);
        writer.u64(message.recovery->native_operation_id);
        writer.u8(message.recovery->manifest_slot);
        writer.digest(message.recovery->candidate_bytes_digest);
    }
    if (message.correlation.has_value()) {
        writer.uuid(message.correlation->product_operation_id);
        writer.uuid(message.correlation->image_id);
        writer.u64(message.correlation->prior_generation);
        writer.u64(message.correlation->candidate_generation);
        writer.u64(message.correlation->native_operation_id);
        writer.u8(message.correlation->manifest_slot);
        writer.digest(message.correlation->candidate_bytes_digest);
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

    Writer writer;
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

    Reader reader(bytes);
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
    if (message.command != WorkerCommand::handshake &&
        !message.product_operation_id.has_value()) {
        return Result<void>::failure(protocol_error(
            static_cast<std::uint32_t>(OperationErrorCode::unknown_request),
            L"authenticate_worker_message",
            L"message does not carry a product operation id"));
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

} // namespace myrd::m5
