#include "src/management/profile_registration_codec.h"

#include "src/common/sha256.h"
#include "src/profile/profile_codec.h"
#include "src/profile/profile_validation.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <string_view>
#include <type_traits>

namespace myrd::management {
namespace {

constexpr std::uint16_t kSchemaMajor = 1;
// Bumped when `CheckpointCommitBoundaryEvidence` gained the manifest slot and
// the candidate digest.  `kMinimumReadableSchemaMinor` is what makes that a
// migration rather than a break: a record written before the bump still decodes,
// with the two fields left at their defaults, and the recovery path refuses on
// them instead of comparing a zero digest as if it were a match.
constexpr std::uint16_t kSchemaMinor = 1;
constexpr std::uint16_t kMinimumReadableSchemaMinor = 0;
constexpr std::size_t kHeaderBytes = 56;
constexpr std::size_t kDigestOffset = 24;
constexpr std::size_t kMaximumWideTextBytes = 32U * 1024U;
constexpr std::size_t kMaximumStableTextBytes = 4U * 1024U;

constexpr std::array<std::byte, 8> kStateMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'P'}, std::byte{'S'}, std::byte{'T'}, std::byte{0}};
constexpr std::array<std::byte, 8> kReservationMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'R'}, std::byte{'S'}, std::byte{'V'}, std::byte{0}};
constexpr std::array<std::byte, 8> kTombstoneMagic{
    std::byte{'M'}, std::byte{'Y'}, std::byte{'R'}, std::byte{'D'},
    std::byte{'T'}, std::byte{'M'}, std::byte{'B'}, std::byte{0}};

template <class T>
Result<T> codec_error(ProfileRegistrationCodecError code,
                      const wchar_t* operation, const wchar_t* message) {
    return Result<T>::failure(Error{ErrorDomain::protocol,
        static_cast<std::uint32_t>(code), operation, message});
}

bool zero_uuid(const product::Uuid& value) noexcept {
    return std::all_of(value.bytes().begin(), value.bytes().end(),
        [](std::byte value) { return value == std::byte{0}; });
}

product::Uuid placeholder_uuid() {
    auto parsed = product::Uuid::parse("00000000-0000-0000-0000-000000000001");
    return parsed.move_value();
}

bool contains_nul(std::string_view value) noexcept {
    return value.find('\0') != std::string_view::npos;
}

class Writer final {
public:
    void u8(std::uint8_t value) { bytes_.push_back(static_cast<std::byte>(value)); }
    void boolean(bool value) { u8(value ? 1U : 0U); }
    void u16(std::uint16_t value) {
        for (unsigned i = 0; i != 2; ++i) u8(static_cast<std::uint8_t>(value >> (8U * i)));
    }
    void u32(std::uint32_t value) {
        for (unsigned i = 0; i != 4; ++i) u8(static_cast<std::uint8_t>(value >> (8U * i)));
    }
    void u64(std::uint64_t value) {
        for (unsigned i = 0; i != 8; ++i) u8(static_cast<std::uint8_t>(value >> (8U * i)));
    }
    void raw(std::span<const std::byte> value) {
        bytes_.insert(bytes_.end(), value.begin(), value.end());
    }
    void array16(std::span<const std::byte, 16> value) {
        u32(16); raw(value);
    }
    void uuid(const product::Uuid& value) { array16(value.bytes()); }
    void optional_uuid(const std::optional<product::Uuid>& value) {
        boolean(value.has_value());
        if (value) uuid(*value);
    }
    void optional_u64(const std::optional<std::uint64_t>& value) {
        boolean(value.has_value());
        if (value) u64(*value);
    }
    void utf8(std::string_view value, std::size_t bound = kMaximumStableTextBytes) {
        if (value.size() > bound || contains_nul(value) || value.size() > UINT32_MAX) {
            good_ = false;
            return;
        }
        if (!valid_utf8(value)) {
            good_ = false;
            return;
        }
        u32(static_cast<std::uint32_t>(value.size()));
        raw(std::as_bytes(std::span(value)));
    }
    void wide(std::wstring_view value) {
        if (value.find(L'\0') != std::wstring_view::npos ||
            value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            good_ = false;
            return;
        }
        if (value.empty()) {
            u32(0);
            return;
        }
        const auto count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (count <= 0 || static_cast<std::size_t>(count) > kMaximumWideTextBytes) {
            good_ = false;
            return;
        }
        std::string encoded(static_cast<std::size_t>(count), '\0');
        if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                static_cast<int>(value.size()), encoded.data(), count, nullptr, nullptr) != count) {
            good_ = false;
            return;
        }
        utf8(encoded, kMaximumWideTextBytes);
    }
    void blob(std::span<const std::byte> value) {
        if (value.size() > UINT32_MAX) { good_ = false; return; }
        u32(static_cast<std::uint32_t>(value.size()));
        raw(value);
    }
    bool good() const noexcept { return good_ && bytes_.size() <= kProfileRegistrationMaximumBytes; }
    const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
    std::vector<std::byte> take() { return std::move(bytes_); }

private:
    static bool valid_utf8(std::string_view value) {
        if (value.empty()) return true;
        if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return false;
        return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), nullptr, 0) > 0;
    }
    bool good_{true};
    std::vector<std::byte> bytes_;
};

class Reader final {
public:
    explicit Reader(std::span<const std::byte> bytes) : bytes_(bytes) {}

    // The schema minor the record was written with.  Defaults to the current one
    // so a reader built without a header -- a nested payload, a test fixture --
    // behaves as the newest format; `unwrap` overwrites it from the record it
    // validated.
    std::uint16_t schema_minor{kSchemaMinor};

    bool u8(std::uint8_t& value) {
        if (remaining() < 1) return fail();
        value = std::to_integer<std::uint8_t>(bytes_[offset_++]);
        return true;
    }
    bool boolean(bool& value) {
        std::uint8_t raw{};
        if (!u8(raw) || raw > 1) return fail();
        value = raw != 0;
        return true;
    }
    bool u16(std::uint16_t& value) { return integer(value); }
    bool u32(std::uint32_t& value) { return integer(value); }
    bool u64(std::uint64_t& value) { return integer(value); }
    bool raw(std::span<std::byte> output) {
        if (remaining() < output.size()) return fail();
        std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), output.size(), output.begin());
        offset_ += output.size();
        return true;
    }
    bool uuid(product::Uuid& value) {
        std::array<std::byte, 16> bytes{};
        if (!array16(bytes)) return false;
        static constexpr char kHex[] = "0123456789abcdef";
        std::string text;
        text.reserve(36);
        for (std::size_t i = 0; i != bytes.size(); ++i) {
            if (i == 4 || i == 6 || i == 8 || i == 10) text.push_back('-');
            const auto byte = std::to_integer<std::uint8_t>(bytes[i]);
            text.push_back(kHex[byte >> 4U]);
            text.push_back(kHex[byte & 0x0FU]);
        }
        auto parsed = product::Uuid::parse(text);
        if (!parsed.ok() || zero_uuid(parsed.value())) return fail();
        value = parsed.move_value();
        return true;
    }
    bool optional_uuid(std::optional<product::Uuid>& value) {
        bool present{};
        if (!boolean(present)) return false;
        if (!present) { value.reset(); return true; }
        auto parsed = placeholder_uuid();
        if (!uuid(parsed)) return false;
        value = std::move(parsed);
        return true;
    }
    bool optional_u64(std::optional<std::uint64_t>& value) {
        bool present{};
        if (!boolean(present)) return false;
        if (!present) { value.reset(); return true; }
        std::uint64_t parsed{};
        if (!u64(parsed)) return false;
        value = parsed;
        return true;
    }
    bool array16(std::span<std::byte, 16> value) {
        std::uint32_t size{};
        return u32(size) && size == value.size() && raw(value);
    }
    bool utf8(std::string& value, std::size_t bound = kMaximumStableTextBytes) {
        std::uint32_t size{};
        if (!u32(size) || size > bound || remaining() < size) return fail();
        const auto data = bytes_.subspan(offset_, size);
        if (std::find(data.begin(), data.end(), std::byte{0}) != data.end()) return fail();
        value.assign(reinterpret_cast<const char*>(data.data()), data.size());
        offset_ += size;
        if (!value.empty() && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                value.data(), static_cast<int>(value.size()), nullptr, 0) <= 0) return fail();
        return true;
    }
    bool wide(std::wstring& value) {
        std::string encoded;
        if (!utf8(encoded, kMaximumWideTextBytes)) return false;
        if (encoded.empty()) { value.clear(); return true; }
        const auto count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            encoded.data(), static_cast<int>(encoded.size()), nullptr, 0);
        if (count <= 0) return fail();
        value.resize(static_cast<std::size_t>(count));
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, encoded.data(),
                static_cast<int>(encoded.size()), value.data(), count) != count) return fail();
        return true;
    }
    bool blob(std::span<const std::byte>& value) {
        std::uint32_t size{};
        if (!u32(size) || remaining() < size) return fail();
        value = bytes_.subspan(offset_, size);
        offset_ += size;
        return true;
    }
    bool done() const noexcept { return good_ && offset_ == bytes_.size(); }

private:
    template <class T> bool integer(T& value) {
        static_assert(std::is_unsigned_v<T>);
        if (remaining() < sizeof(T)) return fail();
        value = 0;
        for (unsigned i = 0; i != sizeof(T); ++i) {
            value |= static_cast<T>(std::to_integer<std::uint8_t>(bytes_[offset_++])) << (8U * i);
        }
        return true;
    }
    std::size_t remaining() const noexcept { return bytes_.size() - offset_; }
    bool fail() noexcept { good_ = false; return false; }
    std::span<const std::byte> bytes_;
    std::size_t offset_{};
    bool good_{true};
};

template <class Enum>
void write_enum(Writer& writer, Enum value) { writer.u8(static_cast<std::uint8_t>(value)); }

template <class Enum>
bool read_enum(Reader& reader, Enum& value, std::uint8_t maximum) {
    std::uint8_t raw{};
    if (!reader.u8(raw) || raw > maximum) return false;
    value = static_cast<Enum>(raw);
    return true;
}

template <class T, class Write>
void write_optional(Writer& writer, const std::optional<T>& value, Write write) {
    writer.boolean(value.has_value());
    if (value) write(writer, *value);
}

template <class T, class Read>
bool read_optional(Reader& reader, std::optional<T>& value, Read read) {
    bool present{};
    if (!reader.boolean(present)) return false;
    if (!present) { value.reset(); return true; }
    auto decoded = [] {
        if constexpr (std::is_same_v<T, CheckpointCommitBoundaryEvidence>) {
            return T{placeholder_uuid(), placeholder_uuid(), placeholder_uuid(),
                0, 0, 0, 0, 0, 0, std::nullopt};
        } else if constexpr (std::is_same_v<T, CompactionMutationBoundaryEvidence>) {
            return T{placeholder_uuid(), placeholder_uuid(), placeholder_uuid(),
                0, 0, 0, 0, 0, 0, 0, 0, 0, 0, std::nullopt, false};
        } else if constexpr (std::is_same_v<T, CompactionPreflightFailureEvidence>) {
            return T{placeholder_uuid(), placeholder_uuid(), placeholder_uuid(),
                0, 0, 0, 0, false};
        } else if constexpr (std::is_same_v<T, BundleRegistrationEvidence>) {
            return T{placeholder_uuid(), "", 0, 0, 0, 0,
                BundleFileIdentity{0, {}}, BundleEvidenceSource::created_and_verified};
        } else if constexpr (std::is_same_v<T, VolumeRegistrationEvidence>) {
            return T{placeholder_uuid(), placeholder_uuid(), placeholder_uuid()};
        } else {
            return T{};
        }
    }();
    if (!read(reader, decoded)) return false;
    value = std::move(decoded);
    return true;
}

void write_profile_state(Writer& w, const ProfileState& v) {
    w.boolean(v.lifecycle.has_value());
    if (v.lifecycle) write_enum(w, *v.lifecycle);
    write_enum(w, v.disk.runtime); write_enum(w, v.disk.activity);
    write_enum(w, v.disk.persistence); write_enum(w, v.disk.health);
}
bool read_profile_state(Reader& r, ProfileState& v) {
    bool lifecycle{};
    if (!r.boolean(lifecycle)) return false;
    if (lifecycle) {
        profile::ProfileLifecycle parsed{};
        if (!read_enum(r, parsed, 1)) return false;
        v.lifecycle = parsed;
    } else v.lifecycle.reset();
    return read_enum(r, v.disk.runtime, 4) && read_enum(r, v.disk.activity, 5)
        && read_enum(r, v.disk.persistence, 3) && read_enum(r, v.disk.health, 3)
        && is_valid(v);
}

bool valid_active_command(ManagementCommand command) noexcept {
    const auto raw = static_cast<std::uint8_t>(command);
    return raw <= static_cast<std::uint8_t>(ManagementCommand::resolve_commit);
}
void write_gate(Writer& w, const OperationGateState& v) {
    w.optional_uuid(v.active_operation_id); write_enum(w, v.active_command); w.boolean(v.cancellable);
}
bool read_gate(Reader& r, OperationGateState& v) {
    if (!r.optional_uuid(v.active_operation_id)
        || !read_enum(r, v.active_command, static_cast<std::uint8_t>(ManagementCommand::operation_cancel))
        || !r.boolean(v.cancellable)) return false;
    if (!v.active_operation_id) return v.active_command == ManagementCommand::status_read && !v.cancellable;
    return valid_active_command(v.active_command);
}

void write_checkpoint_context(Writer& w, const CheckpointPersistenceErrorContext& v) {
    w.utf8(v.operation); w.u32(v.code); w.u8(v.phase); w.u32(v.native_code);
    w.u64(v.native_operation_id); w.array16(v.image_uuid); w.u64(v.generation);
    write_enum(w, v.disposition); w.utf8(v.safe_context);
}
bool read_checkpoint_context(Reader& r, CheckpointPersistenceErrorContext& v) {
    return r.utf8(v.operation) && r.u32(v.code) && r.u8(v.phase) && r.u32(v.native_code)
        && r.u64(v.native_operation_id) && r.array16(v.image_uuid) && r.u64(v.generation)
        && read_enum(r, v.disposition, 3) && r.utf8(v.safe_context);
}
void write_checkpoint_failure(Writer& w, const CheckpointFailureEvidence& v) {
    write_enum(w, v.domain); w.u32(v.code); w.wide(v.operation); w.wide(v.message);
    w.u32(v.native_code); write_checkpoint_context(w, v.context);
}
bool read_checkpoint_failure(Reader& r, CheckpointFailureEvidence& v) {
    return read_enum(r, v.domain, static_cast<std::uint8_t>(ErrorDomain::persistence))
        && r.u32(v.code) && r.wide(v.operation) && r.wide(v.message)
        && r.u32(v.native_code) && read_checkpoint_context(r, v.context);
}
void write_checkpoint_reconciliation(Writer& w, const CheckpointReconciliationFailureEvidence& v) {
    write_enum(w, v.domain); w.u32(v.code); w.wide(v.operation); w.wide(v.message); w.u32(v.native_code);
    write_optional(w, v.unresolved_disposition, [](Writer& out, auto value) { write_enum(out, value); });
    w.utf8(v.safe_context);
}
bool read_checkpoint_reconciliation(Reader& r, CheckpointReconciliationFailureEvidence& v) {
    return read_enum(r, v.domain, static_cast<std::uint8_t>(ErrorDomain::persistence))
        && r.u32(v.code) && r.wide(v.operation) && r.wide(v.message) && r.u32(v.native_code)
        && read_optional(r, v.unresolved_disposition, [](Reader& in, auto& value) { return read_enum(in, value, 3); })
        && r.utf8(v.safe_context);
}
void write_checkpoint_correlation(Writer& w, const CheckpointCommitBoundaryEvidence& v) {
    w.uuid(v.product_operation_id); w.uuid(v.profile_id); w.uuid(v.image_id); w.u32(v.worker_pid);
    w.u64(v.worker_process_creation_time); w.u64(v.worker_session_generation); w.u64(v.prior_generation);
    w.u64(v.candidate_generation); w.u64(v.cut_dirty_epoch); w.optional_u64(v.native_m3_operation_id);
    w.u8(v.manifest_slot); w.raw(v.candidate_bytes_digest);
}
bool read_checkpoint_correlation(Reader& r, CheckpointCommitBoundaryEvidence& v) {
    if (!(r.uuid(v.product_operation_id) && r.uuid(v.profile_id) && r.uuid(v.image_id) && r.u32(v.worker_pid)
        && r.u64(v.worker_process_creation_time) && r.u64(v.worker_session_generation) && r.u64(v.prior_generation)
        && r.u64(v.candidate_generation) && r.u64(v.cut_dirty_epoch) && r.optional_u64(v.native_m3_operation_id))) {
        return false;
    }
    // Added in schema minor 1.  A record written before it simply does not carry
    // these, and the defaults it is left with are the point: a zero digest means
    // "this correlation cannot support a recovery proof", which the recovery
    // path refuses on.  Reading past the end would be an error; inventing a
    // digest that could compare equal would be worse.
    if (r.schema_minor >= 1) {
        if (!r.u8(v.manifest_slot) || !r.raw(v.candidate_bytes_digest)) return false;
    }
    return true;
}
void write_checkpoint(Writer& w, const CheckpointDurableState& v) {
    w.u64(v.confirmed_generation); w.u64(v.current_dirty_epoch); write_enum(w, v.phase);
    write_optional(w, v.before_state, write_profile_state);
    write_optional(w, v.correlation, write_checkpoint_correlation);
    write_optional(w, v.disposition, [](Writer& out, auto value) { write_enum(out, value); });
    write_optional(w, v.failure, write_checkpoint_failure);
    write_optional(w, v.reconciliation_failure, write_checkpoint_reconciliation);
}
bool read_checkpoint(Reader& r, CheckpointDurableState& v) {
    return r.u64(v.confirmed_generation) && r.u64(v.current_dirty_epoch) && read_enum(r, v.phase, 5)
        && read_optional(r, v.before_state, read_profile_state)
        && read_optional(r, v.correlation, read_checkpoint_correlation)
        && read_optional(r, v.disposition, [](Reader& in, auto& value) { return read_enum(in, value, 3); })
        && read_optional(r, v.failure, read_checkpoint_failure)
        && read_optional(r, v.reconciliation_failure, read_checkpoint_reconciliation);
}

void write_compaction_correlation(Writer& w, const CompactionMutationBoundaryEvidence& v) {
    w.uuid(v.product_operation_id); w.uuid(v.profile_id); w.uuid(v.image_id); w.u32(v.worker_pid);
    w.u64(v.worker_process_creation_time); w.u64(v.worker_session_generation); w.u64(v.prior_generation);
    w.u64(v.candidate_generation); w.u64(v.cut_dirty_epoch); w.u64(v.source_base_bytes);
    w.u64(v.source_journal_bytes); w.u64(v.required_peak_free_bytes); w.u64(v.available_free_bytes);
    w.optional_u64(v.native_m3_operation_id); w.boolean(v.cut_dirty_epoch_known);
}
bool read_compaction_correlation(Reader& r, CompactionMutationBoundaryEvidence& v) {
    return r.uuid(v.product_operation_id) && r.uuid(v.profile_id) && r.uuid(v.image_id) && r.u32(v.worker_pid)
        && r.u64(v.worker_process_creation_time) && r.u64(v.worker_session_generation) && r.u64(v.prior_generation)
        && r.u64(v.candidate_generation) && r.u64(v.cut_dirty_epoch) && r.u64(v.source_base_bytes)
        && r.u64(v.source_journal_bytes) && r.u64(v.required_peak_free_bytes) && r.u64(v.available_free_bytes)
        && r.optional_u64(v.native_m3_operation_id) && r.boolean(v.cut_dirty_epoch_known);
}
void write_metrics(Writer& w, const CompactionMetrics& v) {
    w.u64(v.source_generation); w.u64(v.committed_generation); w.u64(v.source_base_bytes);
    w.u64(v.source_journal_bytes); w.u64(v.candidate_base_bytes); w.u64(v.candidate_journal_bytes);
    w.u64(v.required_peak_free_bytes); w.u64(v.available_free_bytes); w.u64(v.live_extent_count);
    w.u64(v.candidate_payloads); w.u64(v.snapshot_records); w.u64(v.data_records); w.u64(v.zero_records);
    w.u64(v.discard_records); w.u64(v.index_nodes); w.u64(v.newer_dirty_preserved);
    w.u64(v.retained_candidate_count); w.u64(v.deleted_candidate_count);
}
bool read_metrics(Reader& r, CompactionMetrics& v) {
    return r.u64(v.source_generation) && r.u64(v.committed_generation) && r.u64(v.source_base_bytes)
        && r.u64(v.source_journal_bytes) && r.u64(v.candidate_base_bytes) && r.u64(v.candidate_journal_bytes)
        && r.u64(v.required_peak_free_bytes) && r.u64(v.available_free_bytes) && r.u64(v.live_extent_count)
        && r.u64(v.candidate_payloads) && r.u64(v.snapshot_records) && r.u64(v.data_records) && r.u64(v.zero_records)
        && r.u64(v.discard_records) && r.u64(v.index_nodes) && r.u64(v.newer_dirty_preserved)
        && r.u64(v.retained_candidate_count) && r.u64(v.deleted_candidate_count);
}
void write_compaction_context(Writer& w, const CompactionPersistenceErrorContext& v) {
    w.utf8(v.operation); w.u32(v.code); w.u8(v.phase); w.u32(v.native_code); w.u64(v.native_operation_id);
    w.array16(v.image_uuid); w.u64(v.generation); write_enum(w, v.disposition); w.utf8(v.safe_context);
}
bool read_compaction_context(Reader& r, CompactionPersistenceErrorContext& v) {
    return r.utf8(v.operation) && r.u32(v.code) && r.u8(v.phase) && r.u32(v.native_code)
        && r.u64(v.native_operation_id) && r.array16(v.image_uuid) && r.u64(v.generation)
        && read_enum(r, v.disposition, 3) && r.utf8(v.safe_context);
}
void write_compaction_failure(Writer& w, const CompactionFailureEvidence& v) {
    write_enum(w, v.domain); w.u32(v.code); w.wide(v.operation); w.wide(v.message);
    w.u32(v.native_code); write_compaction_context(w, v.context);
}
bool read_compaction_failure(Reader& r, CompactionFailureEvidence& v) {
    return read_enum(r, v.domain, static_cast<std::uint8_t>(ErrorDomain::persistence))
        && r.u32(v.code) && r.wide(v.operation) && r.wide(v.message)
        && r.u32(v.native_code) && read_compaction_context(r, v.context);
}
void write_compaction_reconciliation(Writer& w, const CompactionReconciliationFailureEvidence& v) {
    write_enum(w, v.domain); w.u32(v.code); w.wide(v.operation); w.wide(v.message); w.u32(v.native_code);
    write_optional(w, v.unresolved_disposition, [](Writer& out, auto value) { write_enum(out, value); });
    w.utf8(v.safe_context);
}
bool read_compaction_reconciliation(Reader& r, CompactionReconciliationFailureEvidence& v) {
    return read_enum(r, v.domain, static_cast<std::uint8_t>(ErrorDomain::persistence))
        && r.u32(v.code) && r.wide(v.operation) && r.wide(v.message) && r.u32(v.native_code)
        && read_optional(r, v.unresolved_disposition, [](Reader& in, auto& value) { return read_enum(in, value, 3); })
        && r.utf8(v.safe_context);
}
void write_preflight(Writer& w, const CompactionPreflightFailureEvidence& v) {
    w.uuid(v.product_operation_id); w.uuid(v.profile_id); w.uuid(v.image_id); w.u64(v.source_generation);
    w.u64(v.candidate_generation); w.u64(v.required_peak_free_bytes); w.u64(v.available_free_bytes);
    w.boolean(v.future_checkpoint_possible);
}
bool read_preflight(Reader& r, CompactionPreflightFailureEvidence& v) {
    return r.uuid(v.product_operation_id) && r.uuid(v.profile_id) && r.uuid(v.image_id)
        && r.u64(v.source_generation) && r.u64(v.candidate_generation)
        && r.u64(v.required_peak_free_bytes) && r.u64(v.available_free_bytes)
        && r.boolean(v.future_checkpoint_possible);
}
void write_compaction(Writer& w, const CompactionDurableState& v) {
    write_enum(w, v.phase); write_optional(w, v.before_state, write_profile_state);
    write_optional(w, v.correlation, write_compaction_correlation);
    write_optional(w, v.disposition, [](Writer& out, auto value) { write_enum(out, value); });
    write_optional(w, v.metrics, write_metrics); write_optional(w, v.failure, write_compaction_failure);
    write_optional(w, v.reconciliation_failure, write_compaction_reconciliation);
    write_optional(w, v.preflight_failure, write_preflight); w.boolean(v.prerequisite_checkpoint_performed);
    w.u64(v.prerequisite_checkpoint_generation);
    write_optional(w, v.prerequisite_checkpoint_disposition, [](Writer& out, auto value) { write_enum(out, value); });
    w.u64(v.successful_compactions_total); w.u64(v.checkpoints_since_compaction);
    w.optional_uuid(v.last_counted_checkpoint_operation_id); w.optional_uuid(v.last_published_compaction_operation_id);
}
bool read_compaction(Reader& r, CompactionDurableState& v) {
    return read_enum(r, v.phase, 9) && read_optional(r, v.before_state, read_profile_state)
        && read_optional(r, v.correlation, read_compaction_correlation)
        && read_optional(r, v.disposition, [](Reader& in, auto& value) { return read_enum(in, value, 3); })
        && read_optional(r, v.metrics, read_metrics) && read_optional(r, v.failure, read_compaction_failure)
        && read_optional(r, v.reconciliation_failure, read_compaction_reconciliation)
        && read_optional(r, v.preflight_failure, read_preflight) && r.boolean(v.prerequisite_checkpoint_performed)
        && r.u64(v.prerequisite_checkpoint_generation)
        && read_optional(r, v.prerequisite_checkpoint_disposition, [](Reader& in, auto& value) { return read_enum(in, value, 3); })
        && r.u64(v.successful_compactions_total) && r.u64(v.checkpoints_since_compaction)
        && r.optional_uuid(v.last_counted_checkpoint_operation_id)
        && r.optional_uuid(v.last_published_compaction_operation_id);
}

void write_stop(Writer& w, const StopDurableState& v) {
    write_enum(w, v.phase); w.optional_uuid(v.operation_id); write_optional(w, v.before_state, write_profile_state);
    w.wide(v.target); write_enum(w, v.policy); w.boolean(v.force); w.boolean(v.volatile_data_loss_acknowledged);
    w.boolean(v.volatile_data_loss_possible); w.u64(v.confirmed_generation_before); w.u64(v.dirty_epoch_before);
}
bool read_stop(Reader& r, StopDurableState& v) {
    return read_enum(r, v.phase, 9) && r.optional_uuid(v.operation_id)
        && read_optional(r, v.before_state, read_profile_state) && r.wide(v.target)
        && read_enum(r, v.policy, 1) && r.boolean(v.force)
        && r.boolean(v.volatile_data_loss_acknowledged) && r.boolean(v.volatile_data_loss_possible)
        && r.u64(v.confirmed_generation_before) && r.u64(v.dirty_epoch_before);
}

void write_intent(Writer& w, const RecoveryOperationIntent& v) {
    write_enum(w, v.kind); w.uuid(v.profile_id); w.wide(v.bundle_path);
    w.optional_uuid(v.expected_image_id); w.optional_uuid(v.confirmation_id); w.boolean(v.from_previous);
}

bool nonzero_file_identity(const BundleFileIdentity& value) noexcept {
    return value.volume_serial != 0
        && std::any_of(value.file_id.begin(), value.file_id.end(),
            [](std::byte byte) { return byte != std::byte{0}; });
}

bool valid_lifecycle_bundle(const LifecycleOperationIntent& intent,
                            const BundleRegistrationEvidence& evidence) noexcept {
    return evidence.source == (intent.kind == LifecycleOperationKind::create
            ? BundleEvidenceSource::created_and_verified
            : BundleEvidenceSource::read_only_inspection)
        && (!intent.expected_image_id.has_value()
            || evidence.image_id == intent.expected_image_id.value())
        && evidence.logical_capacity_bytes != 0
        && (intent.requested_capacity_bytes == 0
            || evidence.logical_capacity_bytes == intent.requested_capacity_bytes)
        && evidence.logical_sector_bytes == kProductLogicalSectorBytes
        && evidence.physical_sector_bytes == kProductPhysicalSectorBytes
        && evidence.payload_block_bytes == kProductPayloadBlockBytes
        && nonzero_file_identity(evidence.file_identity)
        && !evidence.manifest_name.empty();
}

bool lifecycle_intent_phase_valid(const LifecycleOperationIntent& intent,
                                  LifecycleReservationPhase phase) noexcept {
    if (phase > LifecycleReservationPhase::ready_published) return false;
    if (intent.kind == LifecycleOperationKind::create) {
        return phase != LifecycleReservationPhase::inspection_action_started
            && phase != LifecycleReservationPhase::inspection_verified;
    }
    return phase != LifecycleReservationPhase::bundle_action_started
        && phase != LifecycleReservationPhase::bundle_verified;
}

void write_identity(Writer&, const BundleFileIdentity&);
bool read_identity(Reader&, BundleFileIdentity&);

void write_lifecycle_intent(Writer& w, const LifecycleOperationIntent& v) {
    write_enum(w, v.kind); w.uuid(v.profile_id); w.optional_uuid(v.expected_image_id);
    w.wide(v.owner_sid); w.wide(v.display_name); w.wide(v.bundle_path);
    w.wide(v.mount_target); w.u64(v.requested_capacity_bytes); w.boolean(v.auto_start);
    write_enum(w, v.memory_mode); w.optional_u64(v.memory_hard_limit_bytes);
    write_optional(w, v.checkpoint_interval, [](Writer& out, const auto& value) {
        out.boolean(value.enabled); out.u64(static_cast<std::uint64_t>(value.duration.count()));
    });
    write_optional(w, v.dirty_threshold, [](Writer& out, const auto& value) {
        out.boolean(value.enabled); out.u64(value.bytes);
    });
    w.u64(v.physical_memory.total_bytes); w.u64(v.physical_memory.available_bytes);
    w.u64(v.created_unix_ms);
}

bool read_lifecycle_intent(Reader& r, LifecycleOperationIntent& v) {
    if (!read_enum(r, v.kind, 1) || !r.uuid(v.profile_id)
        || !r.optional_uuid(v.expected_image_id) || !r.wide(v.owner_sid)
        || !r.wide(v.display_name) || !r.wide(v.bundle_path)
        || !r.wide(v.mount_target) || !r.u64(v.requested_capacity_bytes)
        || !r.boolean(v.auto_start) || !read_enum(r, v.memory_mode, 1)
        || !r.optional_u64(v.memory_hard_limit_bytes)) return false;
    if (!read_optional(r, v.checkpoint_interval, [](Reader& in, auto& value) {
            std::uint64_t seconds{}; bool enabled{};
            return in.boolean(enabled) && in.u64(seconds)
                && seconds <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())
                && (value = product::CheckpointInterval{enabled,
                    std::chrono::seconds(seconds)}, true);
        }) || !read_optional(r, v.dirty_threshold, [](Reader& in, auto& value) {
            return in.boolean(value.enabled) && in.u64(value.bytes);
        }) || !r.u64(v.physical_memory.total_bytes)
        || !r.u64(v.physical_memory.available_bytes) || !r.u64(v.created_unix_ms)) return false;
    return !v.owner_sid.empty() && !v.display_name.empty() && !v.bundle_path.empty()
        && !v.mount_target.empty();
}

void write_bundle_evidence(Writer& w, const BundleRegistrationEvidence& v) {
    w.uuid(v.image_id); w.utf8(v.manifest_name); w.u64(v.logical_capacity_bytes);
    w.u32(v.logical_sector_bytes); w.u32(v.physical_sector_bytes); w.u64(v.payload_block_bytes);
    write_identity(w, v.file_identity); write_enum(w, v.source);
}
bool read_bundle_evidence(Reader& r, BundleRegistrationEvidence& v) {
    return r.uuid(v.image_id) && r.utf8(v.manifest_name) && r.u64(v.logical_capacity_bytes)
        && r.u32(v.logical_sector_bytes) && r.u32(v.physical_sector_bytes)
        && r.u64(v.payload_block_bytes) && read_identity(r, v.file_identity)
        && read_enum(r, v.source, 1);
}
void write_volume_evidence(Writer& w, const VolumeRegistrationEvidence& v) {
    w.uuid(v.gpt_disk_id); w.uuid(v.partition_id); w.uuid(v.volume_id);
}
bool read_volume_evidence(Reader& r, VolumeRegistrationEvidence& v) {
    return r.uuid(v.gpt_disk_id) && r.uuid(v.partition_id) && r.uuid(v.volume_id);
}
bool read_intent(Reader& r, RecoveryOperationIntent& v) {
    return read_enum(r, v.kind, 2) && r.uuid(v.profile_id) && r.wide(v.bundle_path)
        && r.optional_uuid(v.expected_image_id) && r.optional_uuid(v.confirmation_id)
        && r.boolean(v.from_previous);
}
void write_identity(Writer& w, const BundleFileIdentity& v) { w.u64(v.volume_serial); w.array16(v.file_id); }
bool read_identity(Reader& r, BundleFileIdentity& v) {
    if (!r.u64(v.volume_serial) || !r.array16(v.file_id) || v.volume_serial == 0) return false;
    return std::any_of(v.file_id.begin(), v.file_id.end(), [](std::byte b) { return b != std::byte{0}; });
}

template <class Magic>
Result<std::vector<std::byte>> wrap(const Magic& magic, Writer& payload,
                                    const wchar_t* operation) {
    if (!payload.good() || payload.bytes().size() > kProfileRegistrationMaximumBytes - kHeaderBytes) {
        return codec_error<std::vector<std::byte>>(ProfileRegistrationCodecError::invalid_size,
            operation, L"Record fields are invalid or exceed the format bound");
    }
    auto digest = sha256_digest(payload.bytes());
    if (!digest.ok()) return Result<std::vector<std::byte>>::failure(digest.error());
    Writer output;
    output.raw(magic); output.u16(kSchemaMajor); output.u16(kSchemaMinor);
    output.u32(static_cast<std::uint32_t>(kHeaderBytes + payload.bytes().size()));
    output.u32(static_cast<std::uint32_t>(payload.bytes().size())); output.u32(0);
    output.raw(digest.value()); output.raw(payload.bytes());
    return Result<std::vector<std::byte>>::success(output.take());
}

// `minor_out` carries the minor the record was written with, because the payload
// below it is not self-describing: a field added in a later minor is present or
// absent depending on it, and the reader has no other way to know which.
template <class Magic>
Result<std::span<const std::byte>> unwrap(std::span<const std::byte> bytes,
                                         const Magic& magic,
                                         const wchar_t* operation,
                                         std::uint16_t& minor_out) {
    if (bytes.size() < kHeaderBytes || bytes.size() > kProfileRegistrationMaximumBytes) {
        return codec_error<std::span<const std::byte>>(ProfileRegistrationCodecError::invalid_size,
            operation, L"Record size is outside the format bound");
    }
    if (!std::equal(magic.begin(), magic.end(), bytes.begin())) {
        return codec_error<std::span<const std::byte>>(ProfileRegistrationCodecError::invalid_magic,
            operation, L"Record magic does not match its record type");
    }
    Reader header(bytes.subspan(8, 16));
    std::uint16_t major{}, minor{}; std::uint32_t total{}, payload_size{}, reserved{};
    if (!header.u16(major) || !header.u16(minor) || !header.u32(total)
        || !header.u32(payload_size) || !header.u32(reserved) || !header.done()) {
        return codec_error<std::span<const std::byte>>(ProfileRegistrationCodecError::invalid_header,
            operation, L"Record header is malformed");
    }
    if (major != kSchemaMajor) {
        return codec_error<std::span<const std::byte>>(ProfileRegistrationCodecError::unsupported_schema,
            operation, L"Record schema major is unsupported");
    }
    // Older minors are readable on purpose: the only widening so far is the
    // checkpoint correlation, and a record that predates it must decode rather
    // than be rejected outright.  What it must *not* do is decode into something
    // that looks recoverable, which is the payload's job, not this gate's.
    if (minor > kSchemaMinor || minor < kMinimumReadableSchemaMinor) {
        return codec_error<std::span<const std::byte>>(ProfileRegistrationCodecError::unsupported_schema,
            operation, L"Record schema minor is unsupported");
    }
    minor_out = minor;
    if (reserved != 0 || total != bytes.size()
        || payload_size != bytes.size() - kHeaderBytes) {
        return codec_error<std::span<const std::byte>>(ProfileRegistrationCodecError::invalid_header,
            operation, L"Record lengths or reserved header fields are invalid");
    }
    const auto payload = bytes.subspan(kHeaderBytes);
    auto digest = sha256_digest(payload);
    if (!digest.ok()) return Result<std::span<const std::byte>>::failure(digest.error());
    if (!std::equal(digest.value().begin(), digest.value().end(), bytes.begin() + kDigestOffset)) {
        return codec_error<std::span<const std::byte>>(ProfileRegistrationCodecError::checksum_mismatch,
            operation, L"Record checksum does not match its payload");
    }
    return Result<std::span<const std::byte>>::success(payload);
}

template <class T>
Result<T> invalid_fields(const wchar_t* operation) {
    return codec_error<T>(ProfileRegistrationCodecError::invalid_fields,
        operation, L"Record fields are truncated, trailing, or semantically invalid");
}

} // namespace

bool validate_durable_profile_state_record(
    const DurableProfileStateRecord& record) noexcept {
    const auto same_lifecycle = [&](const std::optional<ProfileState>& state) {
        return !state.has_value()
            || (is_valid(state.value())
                && state->lifecycle == record.state.lifecycle);
    };
    if (record.revision == 0 || !is_valid(record.state)
        || !record.state.lifecycle.has_value()
        || !same_lifecycle(record.checkpoint.before_state)
        || !same_lifecycle(record.compaction.before_state)
        || !same_lifecycle(record.stop.before_state)) {
        return false;
    }
    if (!record.operation_gate.active_operation_id.has_value()) {
        if (record.operation_gate != idle_operation_gate()) return false;
    } else if (zero_uuid(record.operation_gate.active_operation_id.value())
               || !valid_active_command(record.operation_gate.active_command)) {
        return false;
    }

    const auto& checkpoint = record.checkpoint;
    switch (checkpoint.phase) {
    case CheckpointDurablePhase::stable:
        if ((checkpoint.disposition.has_value()
             || checkpoint.failure.has_value()
             || checkpoint.reconciliation_failure.has_value())
            && !checkpoint.correlation.has_value()) {
            return false;
        }
        break;
    case CheckpointDurablePhase::admitted:
        if (!checkpoint.before_state.has_value()
            || checkpoint.correlation.has_value()
            || checkpoint.disposition.has_value()
            || checkpoint.failure.has_value()
            || checkpoint.reconciliation_failure.has_value()) {
            return false;
        }
        break;
    case CheckpointDurablePhase::commit_boundary:
        if (!checkpoint.before_state.has_value()
            || !checkpoint.correlation.has_value()
            || checkpoint.disposition.has_value()
            || checkpoint.failure.has_value()
            || checkpoint.reconciliation_failure.has_value()) {
            return false;
        }
        break;
    case CheckpointDurablePhase::evidence_recorded:
        if (!checkpoint.before_state.has_value()
            || !checkpoint.correlation.has_value()
            || !checkpoint.disposition.has_value()
            || checkpoint.reconciliation_failure.has_value()) {
            return false;
        }
        break;
    case CheckpointDurablePhase::committed_pending_epoch_confirmation:
        if (!checkpoint.before_state.has_value()
            || !checkpoint.correlation.has_value()
            || checkpoint.disposition
                != CheckpointCommitDisposition::committed
            || checkpoint.failure.has_value()
            || checkpoint.reconciliation_failure.has_value()) {
            return false;
        }
        break;
    case CheckpointDurablePhase::resolving:
        if (!checkpoint.before_state.has_value()
            || !checkpoint.correlation.has_value()
            || (!checkpoint.disposition.has_value()
                && !checkpoint.reconciliation_failure.has_value())) {
            return false;
        }
        break;
    default:
        return false;
    }

    const auto& compaction = record.compaction;
    switch (compaction.phase) {
    case CompactionDurablePhase::stable:
        break;
    case CompactionDurablePhase::admitted:
    case CompactionDurablePhase::checkpointing:
    case CompactionDurablePhase::checkpoint_confirmed:
    case CompactionDurablePhase::preflight_verified:
        if (!compaction.before_state.has_value()
            || compaction.correlation.has_value()) {
            return false;
        }
        break;
    case CompactionDurablePhase::mutation_boundary:
        if (!compaction.before_state.has_value()
            || !compaction.correlation.has_value()
            || compaction.disposition.has_value()
            || compaction.metrics.has_value()) {
            return false;
        }
        break;
    case CompactionDurablePhase::evidence_recorded:
    case CompactionDurablePhase::committed_pending_epoch_confirmation:
    case CompactionDurablePhase::product_truth_published:
        if (!compaction.before_state.has_value()
            || !compaction.correlation.has_value()
            || !compaction.disposition.has_value()
            || !compaction.metrics.has_value()) {
            return false;
        }
        break;
    case CompactionDurablePhase::resolving:
        if (!compaction.before_state.has_value()
            || !compaction.correlation.has_value()) {
            return false;
        }
        break;
    default:
        return false;
    }

    const auto& stop = record.stop;
    const bool known_policy =
        stop.policy == profile::NormalStopPolicy::checkpoint_required
        || stop.policy == profile::NormalStopPolicy::require_clean;
    if (!known_policy) return false;
    if (stop.phase == StopDurablePhase::stable) {
        return !stop.operation_id.has_value()
            && !stop.before_state.has_value() && stop.target.empty()
            && !stop.force && !stop.volatile_data_loss_acknowledged
            && !stop.volatile_data_loss_possible
            && stop.confirmed_generation_before == 0
            && stop.dirty_epoch_before == 0
            && stop.policy == profile::NormalStopPolicy::checkpoint_required;
    }
    if (stop.phase < StopDurablePhase::admitted
        || stop.phase > StopDurablePhase::resolving
        || !stop.operation_id.has_value()
        || zero_uuid(stop.operation_id.value())
        || !stop.before_state.has_value() || stop.target.empty()) {
        return false;
    }
    if (stop.force) {
        if (!stop.volatile_data_loss_acknowledged) return false;
    } else if (stop.volatile_data_loss_acknowledged
               || stop.volatile_data_loss_possible) {
        return false;
    }
    return true;
}

Result<std::vector<std::byte>> encode_profile_state_record(const DurableProfileStateRecord& v) {
    if (!validate_durable_profile_state_record(v)) {
        return invalid_fields<std::vector<std::byte>>(
            L"profile-registration.state.encode");
    }
    Writer payload;
    payload.u64(v.revision); write_profile_state(payload, v.state); write_gate(payload, v.operation_gate);
    write_checkpoint(payload, v.checkpoint); write_compaction(payload, v.compaction); write_stop(payload, v.stop);
    auto encoded = wrap(kStateMagic, payload, L"profile-registration.state.encode");
    if (!encoded.ok()) return encoded;
    auto verified = decode_profile_state_record(encoded.value());
    if (!verified.ok()) return Result<std::vector<std::byte>>::failure(verified.error());
    return encoded;
}

Result<DurableProfileStateRecord> decode_profile_state_record(std::span<const std::byte> bytes) {
    std::uint16_t minor{};
    auto payload = unwrap(bytes, kStateMagic, L"profile-registration.state.decode", minor);
    if (!payload.ok()) return Result<DurableProfileStateRecord>::failure(payload.error());
    Reader reader(payload.value()); reader.schema_minor = minor; DurableProfileStateRecord v;
    if (!reader.u64(v.revision) || v.revision == 0 || !read_profile_state(reader, v.state)
        || !read_gate(reader, v.operation_gate) || !read_checkpoint(reader, v.checkpoint)
        || !read_compaction(reader, v.compaction) || !read_stop(reader, v.stop)
        || !reader.done() || !validate_durable_profile_state_record(v)) {
        return invalid_fields<DurableProfileStateRecord>(L"profile-registration.state.decode");
    }
    return Result<DurableProfileStateRecord>::success(std::move(v));
}

Result<std::vector<std::byte>> encode_recovery_command_reservation(const RecoveryCommandReservation& v) {
    Writer payload; payload.uuid(v.operation_id); payload.uuid(v.request_id); write_intent(payload, v.intent);
    write_profile_state(payload, v.before_state);
    auto profile = profile::encode_profile(v.expected_profile);
    if (!profile.ok()) return Result<std::vector<std::byte>>::failure(profile.error());
    payload.blob(profile.value());
    auto encoded = wrap(kReservationMagic, payload, L"profile-registration.reservation.encode");
    if (!encoded.ok()) return encoded;
    auto verified = decode_recovery_command_reservation(encoded.value());
    if (!verified.ok()) return Result<std::vector<std::byte>>::failure(verified.error());
    return encoded;
}

Result<RecoveryCommandReservation> decode_recovery_command_reservation(std::span<const std::byte> bytes) {
    std::uint16_t minor{};
    auto payload = unwrap(bytes, kReservationMagic, L"profile-registration.reservation.decode", minor);
    if (!payload.ok()) return Result<RecoveryCommandReservation>::failure(payload.error());
    Reader reader(payload.value()); reader.schema_minor = minor;
    auto operation_id = placeholder_uuid();
    auto request_id = placeholder_uuid();
    RecoveryOperationIntent intent{RecoveryOperationKind::repair_profile,
        placeholder_uuid(), L"", std::nullopt, std::nullopt, false};
    ProfileState before_state{};
    std::span<const std::byte> profile_bytes;
    if (!reader.uuid(operation_id) || !reader.uuid(request_id) || !read_intent(reader, intent)
        || !read_profile_state(reader, before_state) || !reader.blob(profile_bytes) || !reader.done()) {
        return invalid_fields<RecoveryCommandReservation>(L"profile-registration.reservation.decode");
    }
    auto profile = profile::decode_profile(profile_bytes);
    if (!profile.ok() || !profile::validate_profile(profile.value()).ok()
        || profile.value().profile.profile_uuid != intent.profile_id) {
        return invalid_fields<RecoveryCommandReservation>(L"profile-registration.reservation.decode");
    }
    const auto expected_lifecycle =
        std::optional<profile::ProfileLifecycle>(profile.value().profile.lifecycle);
    bool intent_valid{};
    switch (intent.kind) {
    case RecoveryOperationKind::repair_profile:
        intent_valid = intent.bundle_path.empty() && intent.from_previous
            && !intent.expected_image_id && !intent.confirmation_id;
        break;
    case RecoveryOperationKind::rebind:
        intent_valid = !intent.bundle_path.empty() && !intent.from_previous
            && intent.expected_image_id.has_value() && !intent.confirmation_id
            && intent.expected_image_id.value()
                == profile.value().profile.image_uuid;
        break;
    case RecoveryOperationKind::forget:
        intent_valid = intent.bundle_path.empty() && !intent.from_previous
            && !intent.expected_image_id && intent.confirmation_id.has_value()
            && intent.confirmation_id.value() == intent.profile_id;
        break;
    }
    if (!intent_valid || before_state.lifecycle != expected_lifecycle) {
        return invalid_fields<RecoveryCommandReservation>(L"profile-registration.reservation.decode");
    }
    return Result<RecoveryCommandReservation>::success(RecoveryCommandReservation{
        std::move(operation_id), std::move(request_id), std::move(intent),
        std::move(before_state), profile.move_value()});
}

Result<std::vector<std::byte>> encode_lifecycle_registration_reservation(
    const LifecycleRegistrationReservation& v) {
    Writer payload;
    payload.u64(v.revision); payload.uuid(v.operation_id); payload.uuid(v.request_id);
    payload.raw(v.argument_digest); write_lifecycle_intent(payload, v.intent);
    write_enum(payload, v.phase);
    write_optional(payload, v.bundle, write_bundle_evidence);
    write_optional(payload, v.volume, write_volume_evidence);
    auto encoded = wrap(kReservationMagic, payload,
        L"profile-registration.lifecycle-reservation.encode");
    if (!encoded.ok()) return encoded;
    auto verified = decode_lifecycle_registration_reservation(encoded.value());
    if (!verified.ok()) return Result<std::vector<std::byte>>::failure(verified.error());
    return encoded;
}

Result<LifecycleRegistrationReservation>
decode_lifecycle_registration_reservation(std::span<const std::byte> bytes) {
    std::uint16_t minor{};
    auto payload = unwrap(bytes, kReservationMagic, L"profile-registration.lifecycle-reservation.decode", minor);
    if (!payload.ok()) return Result<LifecycleRegistrationReservation>::failure(payload.error());
    Reader reader(payload.value()); reader.schema_minor = minor;
    LifecycleRegistrationReservation v{
        1, placeholder_uuid(), placeholder_uuid(), {},
        LifecycleOperationIntent{LifecycleOperationKind::create,
            placeholder_uuid(), std::nullopt, L"", L"", L"", L"", 0,
            false, control::MemoryMode::automatic, std::nullopt,
            std::nullopt, std::nullopt, PhysicalMemorySnapshot{}, 0},
        LifecycleReservationPhase::reserved, std::nullopt, std::nullopt};
    if (!reader.u64(v.revision) || v.revision == 0 || !reader.uuid(v.operation_id)
        || !reader.uuid(v.request_id)
        || !reader.raw(std::span<std::byte>(v.argument_digest))
        || !read_lifecycle_intent(reader, v.intent)
        || !read_enum(reader, v.phase, 7)
        || !read_optional(reader, v.bundle, read_bundle_evidence)
        || !read_optional(reader, v.volume, read_volume_evidence)
        || !reader.done() || v.argument_digest == std::array<std::byte, 32>{}) {
        return invalid_fields<LifecycleRegistrationReservation>(
            L"profile-registration.lifecycle-reservation.decode");
    }
    if (!validate_lifecycle_registration_reservation(v)) {
        return invalid_fields<LifecycleRegistrationReservation>(
            L"profile-registration.lifecycle-reservation.decode");
    }
    return Result<LifecycleRegistrationReservation>::success(std::move(v));
}

bool validate_lifecycle_registration_reservation(
    const LifecycleRegistrationReservation& value) noexcept {
    if (value.revision == 0 || zero_uuid(value.operation_id)
        || zero_uuid(value.request_id)
        || value.argument_digest == std::array<std::byte, 32>{}
        || zero_uuid(value.intent.profile_id)
        || value.intent.owner_sid.empty()
        || value.intent.display_name.empty()
        || value.intent.bundle_path.empty()
        || value.intent.mount_target.empty()
        || value.intent.physical_memory.total_bytes == 0
        || value.intent.physical_memory.available_bytes
            > value.intent.physical_memory.total_bytes
        || value.intent.created_unix_ms == 0
        || !lifecycle_intent_phase_valid(value.intent, value.phase)) {
        return false;
    }
    const bool bundle_required =
        value.phase >= LifecycleReservationPhase::bundle_verified;
    const bool volume_required =
        value.phase >= LifecycleReservationPhase::volume_identity_recorded;
    if ((!bundle_required && value.bundle.has_value())
        || (!volume_required && value.volume.has_value())) return false;
    if (bundle_required
        && (!value.bundle.has_value()
            || !valid_lifecycle_bundle(value.intent, value.bundle.value()))) {
        return false;
    }
    if (volume_required
        && (!value.volume.has_value()
            || zero_uuid(value.volume->gpt_disk_id)
            || zero_uuid(value.volume->partition_id)
            || zero_uuid(value.volume->volume_id))) {
        return false;
    }
    return true;
}

Result<std::vector<std::byte>> encode_forget_tombstone(const ForgetTombstone& v) {
    Writer payload; payload.uuid(v.operation_id); payload.uuid(v.request_id); payload.uuid(v.profile_id);
    payload.uuid(v.image_id); payload.u64(v.profile_revision); payload.u64(v.confirmed_generation);
    payload.wide(v.retained_bundle_path); payload.wide(v.authorization_owner_sid);
    write_optional(payload, v.retained_bundle_identity, write_identity);
    write_profile_state(payload, v.before_state); write_enum(payload, v.phase);
    auto encoded = wrap(kTombstoneMagic, payload, L"profile-registration.tombstone.encode");
    if (!encoded.ok()) return encoded;
    auto verified = decode_forget_tombstone(encoded.value());
    if (!verified.ok()) return Result<std::vector<std::byte>>::failure(verified.error());
    return encoded;
}

Result<ForgetTombstone> decode_forget_tombstone(std::span<const std::byte> bytes) {
    std::uint16_t minor{};
    auto payload = unwrap(bytes, kTombstoneMagic, L"profile-registration.tombstone.decode", minor);
    if (!payload.ok()) return Result<ForgetTombstone>::failure(payload.error());
    Reader reader(payload.value()); reader.schema_minor = minor;
    ForgetTombstone v{placeholder_uuid(), placeholder_uuid(), placeholder_uuid(),
        placeholder_uuid(), 0, 0, L"", L"", std::nullopt, ProfileState{},
        ForgetTombstonePhase::reserved};
    if (!reader.uuid(v.operation_id) || !reader.uuid(v.request_id) || !reader.uuid(v.profile_id)
        || !reader.uuid(v.image_id) || !reader.u64(v.profile_revision) || v.profile_revision == 0
        || !reader.u64(v.confirmed_generation) || !reader.wide(v.retained_bundle_path)
        || !reader.wide(v.authorization_owner_sid)
        || !read_optional(reader, v.retained_bundle_identity, read_identity)
        || !v.retained_bundle_identity.has_value()
        || !read_profile_state(reader, v.before_state) || !read_enum(reader, v.phase, 2) || !reader.done()
        || v.retained_bundle_path.empty() || v.authorization_owner_sid.empty()) {
        return invalid_fields<ForgetTombstone>(L"profile-registration.tombstone.decode");
    }
    return Result<ForgetTombstone>::success(std::move(v));
}

} // namespace myrd::management
