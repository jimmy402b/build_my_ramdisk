#include "src/management/service_response_wire.h"

#include "src/control/ipc_codec.h"
#include "src/control/ipc_frame.h"

#include <utility>

namespace myrd::management {

namespace {

// Response-document tags.  A tag is the only thing that identifies a field, so the
// numbers are part of the wire contract and are never reused.
enum class ResponseTag : std::uint16_t {
    request_id = 1,
    command = 2,
    disposition = 3,
    operation_id = 4,
    operation = 5,
    profiles = 6,
    warnings = 7,
    error = 8,
    mount_target = 9,
};

constexpr std::uint32_t kEncodeErrorCode = 0x2103;
constexpr std::uint32_t kDecodeErrorCode = 0x2104;

// The fields every response must carry, so "the encoder forgot one" and "the
// payload was hand-built" are both caught rather than defaulted.
constexpr std::uint32_t kRequiredFields =
    (1U << 0U) | (1U << 1U) | (1U << 2U);

template <class T>
Result<T> wire_error(const wchar_t* message) {
    return Result<T>::failure(Error{
        ErrorDomain::protocol, kDecodeErrorCode, L"decode_service_response",
        message});
}

template <class T>
Result<T> encode_error(const wchar_t* message) {
    return Result<T>::failure(Error{ErrorDomain::protocol, kEncodeErrorCode,
                                    L"encode_service_response", message});
}

void put_u8(std::vector<std::byte>& bytes, const std::uint8_t value) {
    bytes.push_back(static_cast<std::byte>(value));
}

void put_u16(std::vector<std::byte>& bytes, const std::uint16_t value) {
    bytes.push_back(static_cast<std::byte>(value & 0xffU));
    bytes.push_back(static_cast<std::byte>((value >> 8U) & 0xffU));
}

void put_u32(std::vector<std::byte>& bytes, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void put_u64(std::vector<std::byte>& bytes, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

void put_uuid(std::vector<std::byte>& bytes, const product::Uuid& value) {
    bytes.insert(bytes.end(), value.bytes().begin(), value.bytes().end());
}

void put_text(std::vector<std::byte>& bytes, const std::string& value) {
    for (const unsigned char ch : value) {
        bytes.push_back(static_cast<std::byte>(ch));
    }
}

// A field header plus its value, appended as one unit so a caller cannot write a
// length that disagrees with the bytes that follow it.
void put_field(std::vector<std::byte>& payload, const ResponseTag tag,
               std::vector<std::byte> value) {
    put_u16(payload, static_cast<std::uint16_t>(tag));
    put_u32(payload, static_cast<std::uint32_t>(value.size()));
    payload.insert(payload.end(), value.begin(), value.end());
}

// A cursor that refuses to read past its end.  Every read advances only on success,
// so a failure leaves the caller with an error rather than a half-consumed payload.
class Reader final {
public:
    explicit Reader(const std::span<const std::byte> bytes) noexcept
        : bytes_(bytes) {}

    [[nodiscard]] std::size_t remaining() const noexcept {
        return bytes_.size() - offset_;
    }
    [[nodiscard]] bool at_end() const noexcept {
        return offset_ == bytes_.size();
    }

    bool u8(std::uint8_t& value) {
        if (remaining() < 1) return false;
        value = static_cast<std::uint8_t>(bytes_[offset_]);
        ++offset_;
        return true;
    }

    bool u16(std::uint16_t& value) {
        if (remaining() < 2) return false;
        const auto low = static_cast<std::uint8_t>(bytes_[offset_]);
        const auto high = static_cast<std::uint8_t>(bytes_[offset_ + 1]);
        value = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(low)
            | static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U));
        offset_ += 2;
        return true;
    }

    bool u32(std::uint32_t& value) {
        if (remaining() < 4) return false;
        std::uint32_t result = 0;
        for (unsigned index = 0; index < 4; ++index) {
            result |= static_cast<std::uint32_t>(
                          static_cast<std::uint8_t>(bytes_[offset_ + index]))
                << (index * 8U);
        }
        value = result;
        offset_ += 4;
        return true;
    }

    bool u64(std::uint64_t& value) {
        if (remaining() < 8) return false;
        std::uint64_t result = 0;
        for (unsigned index = 0; index < 8; ++index) {
            result |= static_cast<std::uint64_t>(
                          static_cast<std::uint8_t>(bytes_[offset_ + index]))
                << (index * 8U);
        }
        value = result;
        offset_ += 8;
        return true;
    }

    bool take(const std::size_t count, std::span<const std::byte>& value) {
        if (remaining() < count) return false;
        value = bytes_.subspan(offset_, count);
        offset_ += count;
        return true;
    }

private:
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
};

// `Uuid` offers only `parse` and `random` on purpose -- an identifier that could be
// default-constructed or assembled from arbitrary bytes would be one more way to
// make up an operation identifier.  A wire identifier is therefore rebuilt through
// its canonical text form, which is also the form whose parser validates it.
Result<product::Uuid> read_uuid(Reader& reader) {
    std::span<const std::byte> raw;
    if (!reader.take(16, raw)) {
        return wire_error<product::Uuid>(L"The identifier is truncated");
    }
    const auto hex = [](const std::uint8_t nibble) {
        return static_cast<char>(nibble < 10 ? '0' + nibble
                                             : 'a' + (nibble - 10));
    };
    std::string canonical;
    canonical.reserve(36);
    for (std::size_t index = 0; index < 16; ++index) {
        if (index == 4 || index == 6 || index == 8 || index == 10) {
            canonical.push_back('-');
        }
        const auto byte = static_cast<std::uint8_t>(raw[index]);
        canonical.push_back(hex(static_cast<std::uint8_t>(byte >> 4U)));
        canonical.push_back(hex(static_cast<std::uint8_t>(byte & 0x0fU)));
    }
    auto parsed = product::Uuid::parse(canonical);
    if (!parsed.ok()) {
        return wire_error<product::Uuid>(L"The identifier is not canonical");
    }
    return parsed;
}

constexpr std::size_t kProfileFixedBytes = 6 + 8 + 16 + 1 + 8 + 8 + 2;
constexpr std::size_t kOperationFixedBytes = 16 + 16 + 1 + 1 + 1 + 8 + 8 + 1;
constexpr std::size_t kErrorFixedBytes = 1 + 4 + 4 + 2 + 2;

template <class Enum>
bool enum_in_range(const std::uint8_t raw, const std::uint8_t maximum,
                   Enum& value) {
    if (raw > maximum) return false;
    value = static_cast<Enum>(raw);
    return true;
}

Result<void> encode_profile(std::vector<std::byte>& out,
                            const ServiceProfileProjection& profile) {
    if (profile.mount_target.has_value() && profile.mount_target->empty()) {
        // "Present but empty" and "absent" are different facts, and collapsing them
        // would let a profile claim a mount target of nothing.
        return encode_error<void>(L"A mount target is present but empty");
    }
    if (profile.mount_target.has_value()
        && profile.mount_target->size() > kMaximumWireMountBytes) {
        return encode_error<void>(L"A mount target exceeds its limit");
    }
    put_u8(out, static_cast<std::uint8_t>(profile.lifecycle));
    put_u8(out, static_cast<std::uint8_t>(profile.runtime));
    put_u8(out, static_cast<std::uint8_t>(profile.activity));
    put_u8(out, static_cast<std::uint8_t>(profile.persistence));
    put_u8(out, static_cast<std::uint8_t>(profile.health));
    put_u8(out, static_cast<std::uint8_t>(profile.aggregate));
    put_u64(out, profile.confirmed_generation);
    put_uuid(out, profile.profile_id);
    put_u8(out, profile.mount_target.has_value() ? 1 : 0);
    // Before the length, so the mount length stays the last two bytes of the fixed
    // part -- the property the container's framing depends on.
    put_u64(out, profile.resident_bytes);
    put_u64(out, profile.budget_bytes);
    put_u16(out, static_cast<std::uint16_t>(
                     profile.mount_target.has_value()
                         ? profile.mount_target->size() : 0));
    if (profile.mount_target.has_value()) put_text(out, profile.mount_target.value());
    return Result<void>::success();
}

Result<ServiceProfileProjection> decode_profile(
    const std::span<const std::byte> bytes) {
    Reader reader(bytes);
    std::uint8_t lifecycle = 0;
    std::uint8_t runtime = 0;
    std::uint8_t activity = 0;
    std::uint8_t persistence = 0;
    std::uint8_t health = 0;
    std::uint8_t aggregate = 0;
    if (!reader.u8(lifecycle) || !reader.u8(runtime) || !reader.u8(activity)
        || !reader.u8(persistence) || !reader.u8(health) || !reader.u8(aggregate)) {
        return wire_error<ServiceProfileProjection>(
            L"A profile projection is truncated");
    }
    profile::ProfileLifecycle lifecycle_value{};
    product::RuntimeState runtime_value{};
    product::ActivityState activity_value{};
    product::PersistenceState persistence_value{};
    product::HealthState health_value{};
    AggregateStatus aggregate_value{};
    if (!enum_in_range(lifecycle, 1, lifecycle_value)
        || !enum_in_range(runtime, 4, runtime_value)
        || !enum_in_range(activity, 5, activity_value)
        || !enum_in_range(persistence, 3, persistence_value)
        || !enum_in_range(health, 3, health_value)
        || !enum_in_range(aggregate, 3, aggregate_value)) {
        return wire_error<ServiceProfileProjection>(
            L"A profile projection carries a value outside its range");
    }
    std::uint64_t generation = 0;
    if (!reader.u64(generation)) {
        return wire_error<ServiceProfileProjection>(
            L"A profile projection is truncated");
    }
    auto identifier = read_uuid(reader);
    if (!identifier.ok()) {
        return Result<ServiceProfileProjection>::failure(identifier.error());
    }
    std::uint8_t has_mount = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t budget_bytes = 0;
    std::uint16_t mount_length = 0;
    if (!reader.u8(has_mount) || !reader.u64(resident_bytes)
        || !reader.u64(budget_bytes) || !reader.u16(mount_length)) {
        return wire_error<ServiceProfileProjection>(
            L"A profile projection is truncated");
    }
    if (has_mount > 1 || mount_length > kMaximumWireMountBytes
        || reader.remaining() != mount_length) {
        return wire_error<ServiceProfileProjection>(
            L"A profile mount target is invalid");
    }
    std::optional<std::string> mount_target;
    if (has_mount == 1) {
        if (mount_length == 0) {
            return wire_error<ServiceProfileProjection>(
                L"A profile mount target is empty");
        }
        std::span<const std::byte> raw;
        if (!reader.take(mount_length, raw)) {
            return wire_error<ServiceProfileProjection>(
                L"A profile mount target is truncated");
        }
        std::string text;
        text.reserve(mount_length);
        for (const auto byte : raw) {
            text.push_back(static_cast<char>(static_cast<unsigned char>(byte)));
        }
        mount_target = std::move(text);
    } else if (mount_length != 0) {
        return wire_error<ServiceProfileProjection>(
            L"A profile declares no mount target but carries one");
    }
    return Result<ServiceProfileProjection>::success(ServiceProfileProjection{
        identifier.move_value(), lifecycle_value, runtime_value, activity_value,
        persistence_value, health_value, aggregate_value, generation,
        std::move(mount_target), resident_bytes, budget_bytes});
}

Result<void> encode_operation(std::vector<std::byte>& out,
                              const ServiceOperationProjection& operation) {
    if (operation.phase.size() > kMaximumWirePhaseBytes) {
        return encode_error<void>(L"An operation phase exceeds its limit");
    }
    put_uuid(out, operation.operation_id);
    put_uuid(out, operation.profile_id);
    put_u8(out, static_cast<std::uint8_t>(operation.state));
    put_u8(out, operation.cancellable ? 1 : 0);
    put_u8(out, operation.cancel_requested ? 1 : 0);
    put_u64(out, operation.created_unix_ms);
    put_u64(out, operation.updated_unix_ms);
    put_u8(out, static_cast<std::uint8_t>(operation.phase.size()));
    put_text(out, operation.phase);
    return Result<void>::success();
}

Result<ServiceOperationProjection> decode_operation(
    const std::span<const std::byte> bytes) {
    if (bytes.size() < kOperationFixedBytes) {
        return wire_error<ServiceOperationProjection>(
            L"An operation projection is truncated");
    }
    Reader reader(bytes);
    auto operation_id = read_uuid(reader);
    if (!operation_id.ok()) {
        return Result<ServiceOperationProjection>::failure(operation_id.error());
    }
    auto profile_id = read_uuid(reader);
    if (!profile_id.ok()) {
        return Result<ServiceOperationProjection>::failure(profile_id.error());
    }
    std::uint8_t state = 0;
    std::uint8_t cancellable = 0;
    std::uint8_t cancel_requested = 0;
    if (!reader.u8(state) || !reader.u8(cancellable)
        || !reader.u8(cancel_requested)) {
        return wire_error<ServiceOperationProjection>(
            L"An operation projection is truncated");
    }
    OperationState state_value{};
    if (!enum_in_range(state, 5, state_value) || cancellable > 1
        || cancel_requested > 1) {
        return wire_error<ServiceOperationProjection>(
            L"An operation projection carries a value outside its range");
    }
    std::uint64_t created = 0;
    std::uint64_t updated = 0;
    if (!reader.u64(created) || !reader.u64(updated)) {
        return wire_error<ServiceOperationProjection>(
            L"An operation projection is truncated");
    }
    std::uint8_t phase_length = 0;
    if (!reader.u8(phase_length) || phase_length > kMaximumWirePhaseBytes
        || reader.remaining() != phase_length) {
        return wire_error<ServiceOperationProjection>(
            L"An operation phase is invalid");
    }
    std::span<const std::byte> raw;
    static_cast<void>(reader.take(phase_length, raw));
    std::string phase;
    phase.reserve(phase_length);
    for (const auto byte : raw) {
        phase.push_back(static_cast<char>(static_cast<unsigned char>(byte)));
    }
    return Result<ServiceOperationProjection>::success(ServiceOperationProjection{
        operation_id.move_value(), profile_id.move_value(), state_value,
        std::move(phase), created, updated, cancellable == 1,
        cancel_requested == 1});
}

Result<void> encode_error_field(std::vector<std::byte>& out, const Error& error) {
    auto operation = ipc::utf16_to_utf8(error.operation);
    auto message = ipc::utf16_to_utf8(error.message);
    if (!operation.ok() || !message.ok()) {
        return encode_error<void>(L"An error message is not valid UTF-16");
    }
    if (operation.value().size() > kMaximumWireErrorBytes
        || message.value().size() > kMaximumWireErrorBytes) {
        return encode_error<void>(L"An error message exceeds its limit");
    }
    put_u8(out, static_cast<std::uint8_t>(error.domain));
    put_u32(out, error.code);
    put_u32(out, error.native_code);
    put_u16(out, static_cast<std::uint16_t>(operation.value().size()));
    put_text(out, operation.value());
    put_u16(out, static_cast<std::uint16_t>(message.value().size()));
    put_text(out, message.value());
    return Result<void>::success();
}

Result<Error> decode_error_field(const std::span<const std::byte> bytes) {
    if (bytes.size() < kErrorFixedBytes) {
        return wire_error<Error>(L"A response error is truncated");
    }
    Reader reader(bytes);
    std::uint8_t domain = 0;
    std::uint32_t code = 0;
    std::uint32_t native_code = 0;
    if (!reader.u8(domain) || !reader.u32(code) || !reader.u32(native_code)) {
        return wire_error<Error>(L"A response error is truncated");
    }
    ErrorDomain domain_value{};
    if (!enum_in_range(domain, 9, domain_value)) {
        return wire_error<Error>(L"A response error domain is out of range");
    }
    const auto read_text = [&reader](std::wstring& target) -> bool {
        std::uint16_t length = 0;
        if (!reader.u16(length) || length > kMaximumWireErrorBytes) return false;
        std::span<const std::byte> raw;
        if (!reader.take(length, raw)) return false;
        if (length == 0) return true;
        auto converted = ipc::utf8_to_utf16(raw);
        if (!converted.ok()) return false;
        target = converted.move_value();
        return true;
    };
    std::wstring operation;
    std::wstring message;
    if (!read_text(operation) || !read_text(message)) {
        return wire_error<Error>(L"A response error message is invalid");
    }
    if (!reader.at_end()) {
        return wire_error<Error>(L"A response error carries trailing bytes");
    }
    return Result<Error>::success(
        Error{domain_value, code, std::move(operation), std::move(message),
              native_code});
}

std::optional<std::string> mount_target_of(const profile::Profile& profile) {
    if (profile.mount_target.empty()) return std::nullopt;
    auto converted = ipc::utf16_to_utf8(profile.mount_target);
    // A real Windows path is always valid UTF-16, so this is unreachable for a record
    // this product stored.  Reporting the disk without its mount target is better
    // than failing a list, and far better than printing a lossy replacement for a
    // path that does not exist.
    if (!converted.ok()) return std::nullopt;
    return std::move(converted.value());
}

} // namespace

ServiceProfileProjection project_profile(const ManagedProfileSnapshot& snapshot) {
    const auto reported = reported_profile_state(snapshot);
    ServiceProfileProjection projection{
        snapshot.record.profile.profile_uuid,
        snapshot.record.profile.lifecycle,
        reported.disk.runtime,
        reported.disk.activity,
        reported.disk.persistence,
        reported.disk.health,
        aggregate_status(reported),
        snapshot.checkpoint.confirmed_generation,
        mount_target_of(snapshot.record.profile)};
    // The memory gauges are a live fact about the runtime, so they are carried only
    // when the read path actually observed one.  A durable snapshot has no opinion
    // about them, and manufacturing one would let a stopped disk report memory.
    if (snapshot.runtime_observation.has_value()) {
        projection.resident_bytes = snapshot.runtime_observation->resident_bytes;
        projection.budget_bytes = snapshot.runtime_observation->budget_bytes;
    }
    return projection;
}

ServiceOperationProjection project_operation(const OperationRecord& record) {
    return ServiceOperationProjection{
        record.operation_id,
        record.profile_id,
        record.state,
        record.phase,
        record.created_unix_ms,
        record.updated_unix_ms,
        record.cancellable,
        record.cancel_requested};
}

ServiceResponseWire project_response(const ServiceResponse& response) {
    // Two shapes reach here: an answer built from durable snapshots (what `dispatch`
    // produces) and one that already carries projections.  Deriving from the
    // snapshots when they are present keeps the projection rule in this one file
    // instead of duplicating it at every dispatch site.
    std::vector<ServiceProfileProjection> profiles;
    if (!response.profiles.empty()) {
        profiles.reserve(response.profiles.size());
        for (const auto& snapshot : response.profiles) {
            profiles.push_back(project_profile(snapshot));
        }
    } else if (!response.profile_projections.empty()) {
        profiles = response.profile_projections;
    } else if (response.profile_projection.has_value()) {
        profiles.push_back(response.profile_projection.value());
    }
    const std::optional<ServiceOperationProjection> operation =
        response.operation_projection.has_value()
        ? response.operation_projection
        : (response.operation.has_value()
               ? std::optional<ServiceOperationProjection>(
                     project_operation(response.operation.value()))
               : std::nullopt);
    return ServiceResponseWire{response.request_id,
                               response.command,
                               response.disposition,
                               response.operation_id,
                               operation,
                               std::move(profiles),
                               response.warnings,
                               response.error,
                               response.mount_target};
}

Result<std::vector<std::byte>> encode_service_response_wire(
    const ServiceResponseWire& response) {
    if (response.profiles.size() > kMaximumWireProfiles) {
        return encode_error<std::vector<std::byte>>(
            L"The profile count exceeds its limit");
    }
    if (response.warnings.size() > kMaximumWireWarnings) {
        return encode_error<std::vector<std::byte>>(
            L"The warning count exceeds its limit");
    }
    if (response.command.empty()
        || response.command.size() > kMaximumWireCommandBytes) {
        return encode_error<std::vector<std::byte>>(
            L"The command name is empty or too long");
    }
    if (response.mount_target.has_value()
        && (response.mount_target->empty()
            || response.mount_target->size() > kMaximumWireMountBytes)) {
        return encode_error<std::vector<std::byte>>(
            L"A mount target is empty or too long");
    }

    std::vector<std::byte> payload;
    payload.reserve(256);
    put_u16(payload, kServiceResponseWireMajor);
    put_u16(payload, kServiceResponseWireMinor);
    // Patched once every field has been appended, so the count always describes what
    // is actually in the document rather than what a caller intended to write.
    const auto field_count_offset = payload.size();
    put_u16(payload, 0);
    std::uint16_t field_count = 0;

    {
        std::vector<std::byte> value;
        put_uuid(value, response.request_id);
        put_field(payload, ResponseTag::request_id, std::move(value));
        ++field_count;
    }
    {
        std::vector<std::byte> value;
        put_text(value, response.command);
        put_field(payload, ResponseTag::command, std::move(value));
        ++field_count;
    }
    {
        std::vector<std::byte> value;
        put_u8(value, static_cast<std::uint8_t>(response.disposition));
        put_field(payload, ResponseTag::disposition, std::move(value));
        ++field_count;
    }
    if (response.operation_id.has_value()) {
        std::vector<std::byte> value;
        put_uuid(value, response.operation_id.value());
        put_field(payload, ResponseTag::operation_id, std::move(value));
        ++field_count;
    }
    if (response.operation.has_value()) {
        std::vector<std::byte> value;
        auto encoded = encode_operation(value, response.operation.value());
        if (!encoded.ok()) {
            return Result<std::vector<std::byte>>::failure(encoded.error());
        }
        put_field(payload, ResponseTag::operation, std::move(value));
        ++field_count;
    }
    if (!response.profiles.empty()) {
        std::vector<std::byte> value;
        put_u16(value, static_cast<std::uint16_t>(response.profiles.size()));
        for (const auto& profile : response.profiles) {
            auto encoded = encode_profile(value, profile);
            if (!encoded.ok()) {
                return Result<std::vector<std::byte>>::failure(encoded.error());
            }
        }
        put_field(payload, ResponseTag::profiles, std::move(value));
        ++field_count;
    }
    if (!response.warnings.empty()) {
        std::vector<std::byte> value;
        put_u16(value, static_cast<std::uint16_t>(response.warnings.size()));
        for (const auto warning : response.warnings) {
            put_u8(value, static_cast<std::uint8_t>(warning));
        }
        put_field(payload, ResponseTag::warnings, std::move(value));
        ++field_count;
    }
    if (response.error.has_value()) {
        std::vector<std::byte> value;
        auto encoded = encode_error_field(value, response.error.value());
        if (!encoded.ok()) {
            return Result<std::vector<std::byte>>::failure(encoded.error());
        }
        put_field(payload, ResponseTag::error, std::move(value));
        ++field_count;
    }
    if (response.mount_target.has_value()) {
        std::vector<std::byte> value;
        put_text(value, response.mount_target.value());
        put_field(payload, ResponseTag::mount_target, std::move(value));
        ++field_count;
    }

    payload[field_count_offset] = static_cast<std::byte>(field_count & 0xffU);
    payload[field_count_offset + 1] =
        static_cast<std::byte>((field_count >> 8U) & 0xffU);

    if (payload.size() > ipc::kMaxPayloadBytes) {
        return encode_error<std::vector<std::byte>>(
            L"The encoded response exceeds the frame payload limit");
    }
    return Result<std::vector<std::byte>>::success(std::move(payload));
}

Result<std::vector<std::byte>> encode_service_response(
    const ServiceResponse& response) {
    return encode_service_response_wire(project_response(response));
}

Result<ServiceResponseWire> decode_service_response(
    const std::span<const std::byte> payload,
    const std::uint16_t negotiated_minor) {
    if (payload.size() > ipc::kMaxPayloadBytes) {
        return wire_error<ServiceResponseWire>(
            L"The response payload exceeds its limit");
    }
    Reader reader(payload);
    std::uint16_t major = 0;
    std::uint16_t minor = 0;
    std::uint16_t field_count = 0;
    if (!reader.u16(major) || !reader.u16(minor) || !reader.u16(field_count)) {
        return wire_error<ServiceResponseWire>(L"The response header is truncated");
    }
    if (major != kServiceResponseWireMajor) {
        return wire_error<ServiceResponseWire>(
            L"The response document major is unknown");
    }
    if (minor > kServiceResponseWireMinor || minor > negotiated_minor) {
        return wire_error<ServiceResponseWire>(
            L"The response document is newer than this reader");
    }
    if (field_count == 0 || field_count > kMaximumWireFields) {
        return wire_error<ServiceResponseWire>(
            L"The response field count is invalid");
    }

    std::optional<product::Uuid> request_id;
    std::string command;
    ServiceResponseDisposition disposition{ServiceResponseDisposition::completed};
    std::optional<product::Uuid> operation_id;
    std::optional<ServiceOperationProjection> operation;
    std::vector<ServiceProfileProjection> profiles;
    std::vector<control::IdempotentWarning> warnings;
    std::optional<Error> error;
    std::optional<std::string> mount_target;

    std::uint32_t seen = 0;
    for (std::uint16_t index = 0; index < field_count; ++index) {
        std::uint16_t tag_value = 0;
        std::uint32_t length = 0;
        if (!reader.u16(tag_value) || !reader.u32(length)) {
            return wire_error<ServiceResponseWire>(
                L"A response field header is truncated");
        }
        if (tag_value < static_cast<std::uint16_t>(ResponseTag::request_id)
            || tag_value > static_cast<std::uint16_t>(ResponseTag::mount_target)) {
            return wire_error<ServiceResponseWire>(
                L"A response field tag is unknown");
        }
        const auto bit = static_cast<std::uint32_t>(1U) << (tag_value - 1U);
        if ((seen & bit) != 0) {
            return wire_error<ServiceResponseWire>(
                L"A response field is repeated");
        }
        seen |= bit;
        std::span<const std::byte> value;
        if (!reader.take(length, value)) {
            return wire_error<ServiceResponseWire>(
                L"A response field is truncated");
        }
        switch (static_cast<ResponseTag>(tag_value)) {
        case ResponseTag::request_id: {
            Reader identifier(value);
            auto parsed = read_uuid(identifier);
            if (length != 16 || !parsed.ok() || !identifier.at_end()) {
                return wire_error<ServiceResponseWire>(
                    L"The request identifier is invalid");
            }
            request_id = parsed.move_value();
            break;
        }
        case ResponseTag::command: {
            if (length == 0 || length > kMaximumWireCommandBytes) {
                return wire_error<ServiceResponseWire>(
                    L"The command name is invalid");
            }
            for (const auto byte : value) {
                command.push_back(
                    static_cast<char>(static_cast<unsigned char>(byte)));
            }
            break;
        }
        case ResponseTag::disposition: {
            Reader field(value);
            std::uint8_t raw = 0;
            if (length != 1 || !field.u8(raw)
                || !field.at_end()
                || !enum_in_range(raw, 4, disposition)) {
                return wire_error<ServiceResponseWire>(
                    L"The disposition is invalid");
            }
            break;
        }
        case ResponseTag::operation_id: {
            Reader identifier(value);
            auto parsed = read_uuid(identifier);
            if (length != 16 || !parsed.ok() || !identifier.at_end()) {
                return wire_error<ServiceResponseWire>(
                    L"The operation identifier is invalid");
            }
            operation_id = parsed.move_value();
            break;
        }
        case ResponseTag::operation: {
            auto decoded = decode_operation(value);
            if (!decoded.ok()) {
                return Result<ServiceResponseWire>::failure(decoded.error());
            }
            operation = decoded.move_value();
            break;
        }
        case ResponseTag::profiles: {
            Reader container(value);
            std::uint16_t count = 0;
            if (!container.u16(count) || count == 0
                || count > kMaximumWireProfiles) {
                return wire_error<ServiceResponseWire>(
                    L"The profile count is invalid");
            }
            profiles.reserve(count);
            for (std::uint16_t position = 0; position < count; ++position) {
                if (container.remaining() < kProfileFixedBytes) {
                    return wire_error<ServiceResponseWire>(
                        L"A profile projection is truncated");
                }
                std::span<const std::byte> fixed;
                static_cast<void>(container.take(kProfileFixedBytes, fixed));
                // The mount length is the last two bytes of the fixed part and the
                // mount bytes follow it, so the record's boundary is only known once
                // that field has been read.  This is the one place the layout is
                // stepped through by hand, which is why the fixed-size constant is
                // derived from the same field list the encoder writes.
                const auto low = static_cast<std::uint8_t>(fixed[fixed.size() - 2]);
                const auto high = static_cast<std::uint8_t>(fixed[fixed.size() - 1]);
                const auto mount_length = static_cast<std::uint16_t>(
                    static_cast<std::uint16_t>(low)
                    | static_cast<std::uint16_t>(
                          static_cast<std::uint16_t>(high) << 8U));
                if (mount_length > kMaximumWireMountBytes
                    || container.remaining() < mount_length) {
                    return wire_error<ServiceResponseWire>(
                        L"A profile mount target is invalid");
                }
                std::span<const std::byte> mount;
                static_cast<void>(container.take(mount_length, mount));
                std::vector<std::byte> record(fixed.begin(), fixed.end());
                record.insert(record.end(), mount.begin(), mount.end());
                auto decoded = decode_profile(record);
                if (!decoded.ok()) {
                    return Result<ServiceResponseWire>::failure(decoded.error());
                }
                profiles.push_back(decoded.move_value());
            }
            if (!container.at_end()) {
                return wire_error<ServiceResponseWire>(
                    L"The profile list carries trailing bytes");
            }
            break;
        }
        case ResponseTag::warnings: {
            Reader container(value);
            std::uint16_t count = 0;
            if (!container.u16(count) || count == 0
                || count > kMaximumWireWarnings) {
                return wire_error<ServiceResponseWire>(
                    L"The warning count is invalid");
            }
            warnings.reserve(count);
            for (std::uint16_t position = 0; position < count; ++position) {
                std::uint8_t raw = 0;
                control::IdempotentWarning warning{};
                if (!container.u8(raw) || !enum_in_range(raw, 2, warning)) {
                    return wire_error<ServiceResponseWire>(
                        L"A warning value is invalid");
                }
                warnings.push_back(warning);
            }
            if (!container.at_end()) {
                return wire_error<ServiceResponseWire>(
                    L"The warning list carries trailing bytes");
            }
            break;
        }
        case ResponseTag::error: {
            auto decoded = decode_error_field(value);
            if (!decoded.ok()) {
                return Result<ServiceResponseWire>::failure(decoded.error());
            }
            error = decoded.move_value();
            break;
        }
        case ResponseTag::mount_target: {
            if (length == 0 || length > kMaximumWireMountBytes) {
                return wire_error<ServiceResponseWire>(
                    L"The mount target is empty or too long");
            }
            std::string target;
            target.reserve(length);
            for (const auto byte : value) {
                target.push_back(
                    static_cast<char>(static_cast<unsigned char>(byte)));
            }
            mount_target = std::move(target);
            break;
        }
        }
    }

    if (!reader.at_end()) {
        return wire_error<ServiceResponseWire>(
            L"The response carries trailing bytes");
    }
    if ((seen & kRequiredFields) != kRequiredFields) {
        return wire_error<ServiceResponseWire>(
            L"A required response field is missing");
    }
    return Result<ServiceResponseWire>::success(ServiceResponseWire{
        request_id.value(), command, disposition, operation_id, operation,
        std::move(profiles), std::move(warnings), error, std::move(mount_target)});
}

} // namespace myrd::management
