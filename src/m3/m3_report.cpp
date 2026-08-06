#include "m3_report.h"

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <filesystem>
#include <sstream>
#include <string_view>

namespace myrd::m3 {
namespace {

class Json final {
public:
    void object_begin() { value("{"); ++depth_; comma_ = false; }
    void object_end() { --depth_; newline(); output_ += '}'; comma_ = true; }
    void array_begin() { value("["); ++depth_; comma_ = false; }
    void array_end() { --depth_; newline(); output_ += ']'; comma_ = true; }
    void key(std::string_view name) {
        separator();
        newline();
        quoted(name);
        output_ += ": ";
        comma_ = false;
    }
    void string(std::string_view text) { separator(); quoted(text); comma_ = true; }
    void boolean(bool value) { scalar(value ? "true" : "false"); }
    void unsigned_value(std::uint64_t value) {
        char bytes[32]{};
        const auto converted = std::to_chars(
            std::begin(bytes), std::end(bytes), value);
        scalar(std::string_view{bytes, converted.ptr});
    }
    void null() { scalar("null"); }
    std::string finish() { output_ += '\n'; return std::move(output_); }

private:
    void value(std::string_view text) { separator(); output_ += text; }
    void scalar(std::string_view text) {
        separator(); output_ += text; comma_ = true;
    }
    void separator() { if (comma_) output_ += ','; }
    void newline() {
        output_ += '\n';
        output_.append(static_cast<std::size_t>(depth_) * 2, ' ');
    }
    void quoted(std::string_view text) {
        output_ += '"';
        for (const unsigned char byte : text) {
            switch (byte) {
            case '"': output_ += "\\\""; break;
            case '\\': output_ += "\\\\"; break;
            case '\b': output_ += "\\b"; break;
            case '\f': output_ += "\\f"; break;
            case '\n': output_ += "\\n"; break;
            case '\r': output_ += "\\r"; break;
            case '\t': output_ += "\\t"; break;
            default:
                if (byte < 0x20) {
                    constexpr char hex[] = "0123456789ABCDEF";
                    output_ += "\\u00";
                    output_ += hex[(byte >> 4) & 0x0F];
                    output_ += hex[byte & 0x0F];
                } else {
                    output_ += static_cast<char>(byte);
                }
            }
        }
        output_ += '"';
    }

    std::string output_{};
    int depth_{};
    bool comma_{};
};

Error invalid_report(const wchar_t* message) {
    return {ErrorDomain::evidence, 1, L"serialize_m3_report", message};
}

bool is_hex(std::string_view value, std::size_t length) noexcept {
    return value.size() == length &&
        std::all_of(value.begin(), value.end(), [](unsigned char byte) {
            return (byte >= '0' && byte <= '9') ||
                   (byte >= 'a' && byte <= 'f') ||
                   (byte >= 'A' && byte <= 'F');
        });
}

bool valid_utf8(std::string_view value) noexcept {
    std::size_t index{};
    const auto continuation = [&](std::size_t at) {
        return at < value.size() &&
            (static_cast<unsigned char>(value[at]) & 0xC0U) == 0x80U;
    };
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) { ++index; continue; }
        if (first >= 0xC2U && first <= 0xDFU && continuation(index + 1)) {
            index += 2; continue;
        }
        if (index + 2 < value.size() && continuation(index + 2)) {
            const auto second = static_cast<unsigned char>(value[index + 1]);
            if (((first >= 0xE1U && first <= 0xECU) ||
                 (first >= 0xEEU && first <= 0xEFU)) &&
                (second & 0xC0U) == 0x80U) {
                index += 3; continue;
            }
            if (first == 0xE0U && second >= 0xA0U && second <= 0xBFU) {
                index += 3; continue;
            }
            if (first == 0xEDU && second >= 0x80U && second <= 0x9FU) {
                index += 3; continue;
            }
        }
        if (index + 3 < value.size() && continuation(index + 2) &&
            continuation(index + 3)) {
            const auto second = static_cast<unsigned char>(value[index + 1]);
            if (first >= 0xF1U && first <= 0xF3U &&
                (second & 0xC0U) == 0x80U) {
                index += 4; continue;
            }
            if (first == 0xF0U && second >= 0x90U && second <= 0xBFU) {
                index += 4; continue;
            }
            if (first == 0xF4U && second >= 0x80U && second <= 0x8FU) {
                index += 4; continue;
            }
        }
        return false;
    }
    return true;
}

std::optional<std::string_view> operation_name(ReportOperation value) noexcept {
    switch (value) {
    case ReportOperation::Inspect: return "inspect";
    case ReportOperation::VerifyStructural: return "verify_structural";
    case ReportOperation::VerifyFull: return "verify_full";
    }
    return std::nullopt;
}

std::optional<std::string_view> report_status_name(ReportStatus value) noexcept {
    switch (value) {
    case ReportStatus::Passed: return "passed";
    case ReportStatus::Failed: return "failed";
    case ReportStatus::Blocked: return "blocked";
    }
    return std::nullopt;
}

std::optional<std::string_view> slot_status_name(SlotStatus value) noexcept {
    switch (value) {
    case SlotStatus::Empty: return "empty";
    case SlotStatus::Selected: return "selected";
    case SlotStatus::ValidNotSelected: return "valid_not_selected";
    case SlotStatus::FallbackRejected: return "fallback_rejected";
    case SlotStatus::Rejected: return "rejected";
    }
    return std::nullopt;
}

std::optional<std::string_view> depth_name(VerificationDepth value) noexcept {
    switch (value) {
    case VerificationDepth::Structural: return "structural";
    case VerificationDepth::Full: return "full";
    }
    return std::nullopt;
}

bool known_check_status(std::string_view value) noexcept {
    constexpr std::string_view names[] = {
        "not_run", "passed", "missing", "corrupt", "unsupported",
        "access_denied", "io_error", "cancelled", "deadline_exceeded",
        "conflict"};
    return std::find(std::begin(names), std::end(names), value) !=
        std::end(names);
}

bool known_check(std::string_view value) noexcept {
    constexpr std::string_view names[] = {
        "slot_decode", "slot_support", "slot_semantics", "base_path",
        "journal_path", "base_presence", "base_open", "base_identity",
        "base_expected_length", "base_profile", "base_layout",
        "journal_presence", "journal_open", "journal_identity",
        "journal_header", "journal_committed_length", "journal_commit",
        "journal_root", "journal_digest"};
    return std::find(std::begin(names), std::end(names), value) !=
        std::end(names);
}

bool known_cleanup(const CleanupCandidateEvidence& entry) noexcept {
    const bool kind = entry.kind == "base" || entry.kind == "journal";
    constexpr std::string_view decisions[] = {
        "Eligible", "InvalidName", "ReparsePoint", "HardLinked",
        "UnknownOwnership", "ManifestReferenced", "LiveViewReferenced",
        "InFlightReferenced"};
    const bool decision = std::find(
        std::begin(decisions), std::end(decisions), entry.decision) !=
        std::end(decisions);
    const bool safe_name = !entry.relative_name.empty() &&
        entry.relative_name != "." && entry.relative_name != ".." &&
        entry.relative_name.find('/') == std::string::npos &&
        entry.relative_name.find('\\') == std::string::npos &&
        entry.relative_name.find(':') == std::string::npos;
    return kind && safe_name && decision &&
        (entry.action == "Kept" || entry.action == "Deleted");
}

std::string_view check_name(
    persistence::ManifestCandidateCheck value) noexcept {
    using persistence::ManifestCandidateCheck;
    switch (value) {
    case ManifestCandidateCheck::SlotDecode: return "slot_decode";
    case ManifestCandidateCheck::SlotSupport: return "slot_support";
    case ManifestCandidateCheck::SlotSemantics: return "slot_semantics";
    case ManifestCandidateCheck::BasePath: return "base_path";
    case ManifestCandidateCheck::JournalPath: return "journal_path";
    case ManifestCandidateCheck::BasePresence: return "base_presence";
    case ManifestCandidateCheck::BaseOpen: return "base_open";
    case ManifestCandidateCheck::BaseIdentity: return "base_identity";
    case ManifestCandidateCheck::BaseExpectedLength:
        return "base_expected_length";
    case ManifestCandidateCheck::BaseProfile: return "base_profile";
    case ManifestCandidateCheck::BaseLayout: return "base_layout";
    case ManifestCandidateCheck::JournalPresence: return "journal_presence";
    case ManifestCandidateCheck::JournalOpen: return "journal_open";
    case ManifestCandidateCheck::JournalIdentity: return "journal_identity";
    case ManifestCandidateCheck::JournalHeader: return "journal_header";
    case ManifestCandidateCheck::JournalCommittedLength:
        return "journal_committed_length";
    case ManifestCandidateCheck::JournalCommit: return "journal_commit";
    case ManifestCandidateCheck::JournalRoot: return "journal_root";
    case ManifestCandidateCheck::JournalDigest: return "journal_digest";
    case ManifestCandidateCheck::Count: break;
    }
    return "unknown";
}

std::string_view check_status_name(
    persistence::ManifestCandidateCheckStatus value) noexcept {
    using persistence::ManifestCandidateCheckStatus;
    switch (value) {
    case ManifestCandidateCheckStatus::NotRun: return "not_run";
    case ManifestCandidateCheckStatus::Passed: return "passed";
    case ManifestCandidateCheckStatus::Missing: return "missing";
    case ManifestCandidateCheckStatus::Corrupt: return "corrupt";
    case ManifestCandidateCheckStatus::Unsupported: return "unsupported";
    case ManifestCandidateCheckStatus::AccessDenied: return "access_denied";
    case ManifestCandidateCheckStatus::IoError: return "io_error";
    case ManifestCandidateCheckStatus::Cancelled: return "cancelled";
    case ManifestCandidateCheckStatus::DeadlineExceeded:
        return "deadline_exceeded";
    case ManifestCandidateCheckStatus::Conflict: return "conflict";
    }
    return "unknown";
}

std::string_view domain_name(ErrorDomain value) noexcept {
    switch (value) {
    case ErrorDomain::usage: return "usage";
    case ErrorDomain::preflight: return "preflight";
    case ErrorDomain::protocol: return "protocol";
    case ErrorDomain::win32: return "win32";
    case ErrorDomain::ntstatus: return "ntstatus";
    case ErrorDomain::storage: return "storage";
    case ErrorDomain::cleanup: return "cleanup";
    case ErrorDomain::evidence: return "evidence";
    case ErrorDomain::internal: return "internal";
    case ErrorDomain::persistence: return "persistence";
    }
    return "unknown";
}

template <std::size_t Size>
std::string hex_bytes(const std::array<std::byte, Size>& bytes) {
    constexpr char digits[] = "0123456789ABCDEF";
    std::string output(Size * 2, '0');
    for (std::size_t index = 0; index < Size; ++index) {
        const auto value = std::to_integer<unsigned char>(bytes[index]);
        output[index * 2] = digits[(value >> 4U) & 0x0FU];
        output[index * 2 + 1] = digits[value & 0x0FU];
    }
    return output;
}

std::string fixed_hex(std::uint64_t value, int width) {
    std::ostringstream stream;
    stream << std::uppercase << std::hex << std::setw(width)
           << std::setfill('0') << value;
    return stream.str();
}

std::string windows_identity(
    const persistence::WindowsFileIdentity& identity) {
    return fixed_hex(identity.volume_serial_number, 16) + ":" +
        hex_bytes(identity.file_id) + ":" + fixed_hex(identity.size, 16) +
        ":" + fixed_hex(identity.attributes, 8) + ":" +
        fixed_hex(identity.reparse_tag, 8) + ":" +
        fixed_hex(identity.hard_link_count, 8);
}

ErrorEvidence error_evidence(const Error& error, std::string phase) {
    std::string stable = "code_" + std::to_string(error.code);
    if (error.domain == ErrorDomain::persistence) {
        stable = std::string(persistence::to_string(
            static_cast<persistence::PersistenceErrorCode>(error.code)));
    }
    return {std::string(domain_name(error.domain)), error.code,
            error.native_code, std::move(phase), std::move(stable)};
}

void apply_context(M3Report& report, M3ReportContext context) {
    report.operation_id = std::move(context.operation_id);
    report.started_utc = std::move(context.started_utc);
    report.finished_utc = std::move(context.finished_utc);
    report.build = std::move(context.build);
    report.bundle_path_hash = std::move(context.bundle_path_hash);
    report.bundle_path = std::move(context.bundle_path);
    report.include_absolute_path = context.include_absolute_path;
    report.cleanup_candidates = std::move(context.cleanup_candidates);
    report.artifacts = std::move(context.artifacts);
}

void apply_inspection(M3Report& report,
                      const persistence::ImageInspection& inspection,
                      std::string_view phase) {
    std::optional<std::uint64_t> selected_generation{};
    if (inspection.selected.has_value()) {
        const auto& selected = *inspection.selected;
        selected_generation = selected.checkpoint_generation;
        report.image = ImageEvidence{
            hex_bytes(selected.image_uuid.bytes),
            selected.geometry.virtual_bytes,
            selected.geometry.logical_sector_size,
            selected.geometry.physical_sector_size};
        const auto& identity = selected.backing_identity;
        report.selected = SelectedGenerationEvidence{
            selected.selected_slot, selected.fallback,
            selected.checkpoint_generation, selected.base_generation,
            selected.journal_epoch,
            windows_identity(identity.base_windows_identity),
            hex_bytes(identity.base_vhdx_identity.file_write_guid) + ":" +
                hex_bytes(identity.base_vhdx_identity.data_write_guid),
            windows_identity(identity.journal_windows_identity),
            hex_bytes(identity.journal_identity.journal_uuid),
            identity.journal_committed_length,
            {identity.root.offset, identity.root.length, identity.root.crc32c},
            {identity.commit.offset, identity.commit.length,
             identity.commit.crc32c,
             hex_bytes(identity.commit.chain_digest)}};
    }
    for (std::size_t index = 0; index < inspection.diagnostics.size(); ++index) {
        const auto& source = inspection.diagnostics[index];
        auto status = SlotStatus::Rejected;
        if (source.selected) status = SlotStatus::Selected;
        else if (source.status ==
                 persistence::ManifestCandidateCheckStatus::Passed) {
            status = SlotStatus::ValidNotSelected;
        } else if (!source.generation.has_value() &&
                   source.status ==
                       persistence::ManifestCandidateCheckStatus::Missing) {
            status = SlotStatus::Empty;
        } else if (selected_generation.has_value() &&
                   source.generation.has_value() &&
                   *source.generation > *selected_generation) {
            status = SlotStatus::FallbackRejected;
        }
        report.slots[index] = SlotEvidence{
            index == 0 ? "A" : "B", status, source.generation,
            source.first_failed_check.has_value()
                ? std::optional<std::string>{std::string(
                    check_name(*source.first_failed_check))}
                : std::nullopt,
            std::string(check_status_name(source.status)), source.selected};
    }
    if (inspection.rejection.has_value()) {
        report.error = error_evidence(*inspection.rejection,
                                      std::string(phase));
    }
}

Result<void> validate(const M3Report& report) {
    if (report.schema_version != kM3ReportSchemaVersion ||
        !operation_name(report.operation).has_value() ||
        !report_status_name(report.status).has_value() ||
        report.operation_id.empty() || report.started_utc.empty() ||
        report.finished_utc.empty() ||
        !is_hex(report.build.source_commit, 40) ||
        report.build.build_type.empty() ||
        !is_hex(report.build.binary_sha256, 64) ||
        !is_hex(report.bundle_path_hash, 64)) {
        return Result<void>::failure(invalid_report(
            L"required schema or build identity is invalid"));
    }
    if (!valid_utf8(report.operation_id) ||
        !valid_utf8(report.started_utc) || !valid_utf8(report.finished_utc) ||
        !valid_utf8(report.build.build_type) ||
        !valid_utf8(report.bundle_path)) {
        return Result<void>::failure(invalid_report(
            L"report text is not valid UTF-8"));
    }
    if (report.include_absolute_path && report.bundle_path.empty()) {
        return Result<void>::failure(invalid_report(
            L"absolute path opt-in requires a path"));
    }
    if (report.include_absolute_path) {
        try {
            if (!std::filesystem::path(report.bundle_path).is_absolute()) {
                return Result<void>::failure(invalid_report(
                    L"path opt-in requires an absolute path"));
            }
        } catch (const std::filesystem::filesystem_error&) {
            return Result<void>::failure(invalid_report(
                L"path opt-in contains an invalid path"));
        }
    }
    if (report.image.has_value() &&
        (!is_hex(report.image->image_uuid, 32) ||
         report.image->virtual_bytes == 0 ||
         report.image->logical_sector_size == 0 ||
         report.image->physical_sector_size == 0)) {
        return Result<void>::failure(invalid_report(L"image evidence is invalid"));
    }
    for (std::size_t index = 0; index < report.slots.size(); ++index) {
        const auto& slot = report.slots[index];
        if (slot.slot != (index == 0 ? "A" : "B") ||
            !valid_utf8(slot.slot) || !valid_utf8(slot.check_status) ||
            (slot.first_failed_check.has_value() &&
             !valid_utf8(*slot.first_failed_check)) ||
            !slot_status_name(slot.status).has_value() ||
            !known_check_status(slot.check_status) ||
            (slot.first_failed_check.has_value() &&
             !known_check(*slot.first_failed_check))) {
            return Result<void>::failure(invalid_report(
                L"slot evidence contains an unknown value"));
        }
    }
    if (report.selected.has_value()) {
        const auto& selected = *report.selected;
        if (!report.image.has_value() || selected.slot_index > 1 ||
            selected.base_windows_identity.empty() ||
            selected.base_format_identity.empty() ||
            selected.journal_windows_identity.empty() ||
            selected.journal_format_identity.empty() ||
            !valid_utf8(selected.base_windows_identity) ||
            !valid_utf8(selected.base_format_identity) ||
            !valid_utf8(selected.journal_windows_identity) ||
            !valid_utf8(selected.journal_format_identity) ||
            !is_hex(selected.commit.chain_sha256, 64)) {
            return Result<void>::failure(invalid_report(
                L"selected generation evidence is invalid"));
        }
    }
    if (report.verification.has_value()) {
        const auto& verification = *report.verification;
        if (!depth_name(verification.depth).has_value() ||
            verification.data_blocks + verification.zero_blocks !=
                verification.candidate_blocks ||
            (verification.depth == VerificationDepth::Full &&
             !is_hex(verification.logical_sha256, 64))) {
            return Result<void>::failure(invalid_report(
                L"verification evidence is invalid"));
        }
    }
    if (!std::all_of(report.cleanup_candidates.begin(),
                     report.cleanup_candidates.end(),
            [](const CleanupCandidateEvidence& entry) {
                return known_cleanup(entry) && valid_utf8(entry.kind) &&
                    valid_utf8(entry.relative_name) &&
                    valid_utf8(entry.decision) && valid_utf8(entry.action);
            })) {
        return Result<void>::failure(invalid_report(
            L"cleanup evidence contains an unknown value"));
    }
    if (report.artifacts.empty() ||
        !std::all_of(report.artifacts.begin(), report.artifacts.end(),
            [](const ArtifactEvidence& artifact) {
                return !artifact.role.empty() && valid_utf8(artifact.role) &&
                    is_hex(artifact.sha256, 64);
            })) {
        return Result<void>::failure(invalid_report(
            L"artifact hashes are missing or invalid"));
    }
    if (report.status == ReportStatus::Passed && report.error.has_value()) {
        return Result<void>::failure(invalid_report(
            L"a passing report cannot contain a primary error"));
    }
    if (report.status != ReportStatus::Passed && !report.error.has_value()) {
        return Result<void>::failure(invalid_report(
            L"a non-passing report requires a primary error"));
    }
    if (report.error.has_value() &&
        (!valid_utf8(report.error->domain) ||
         !valid_utf8(report.error->phase) ||
         !valid_utf8(report.error->stable_code) ||
         report.error->domain.empty() || report.error->phase.empty() ||
         report.error->stable_code.empty())) {
        return Result<void>::failure(invalid_report(
            L"error evidence contains invalid text"));
    }
    if (report.error.has_value() &&
        report.error->domain != "usage" &&
        report.error->domain != "preflight" &&
        report.error->domain != "protocol" &&
        report.error->domain != "win32" &&
        report.error->domain != "ntstatus" &&
        report.error->domain != "storage" &&
        report.error->domain != "cleanup" &&
        report.error->domain != "evidence" &&
        report.error->domain != "internal" &&
        report.error->domain != "persistence") {
        return Result<void>::failure(invalid_report(
            L"error evidence contains an unknown domain"));
    }
    return Result<void>::success();
}

void optional_uint(Json& json, const std::optional<std::uint64_t>& value) {
    if (value.has_value()) json.unsigned_value(*value); else json.null();
}

void optional_string(Json& json, const std::optional<std::string>& value) {
    if (value.has_value()) json.string(*value); else json.null();
}

} // namespace

Result<M3Report> make_m3_report(
    const persistence::ImageInspection& inspection,
    M3ReportContext context) {
    M3Report report{};
    report.operation = ReportOperation::Inspect;
    report.status = inspection.selected.has_value()
        ? ReportStatus::Passed : ReportStatus::Failed;
    apply_context(report, std::move(context));
    apply_inspection(report, inspection, "inspect");
    if (!inspection.selected.has_value() && !report.error.has_value()) {
        return Result<M3Report>::failure(invalid_report(
            L"rejected inspection has no provenance"));
    }
    return Result<M3Report>::success(std::move(report));
}

Result<M3Report> make_m3_report(
    const persistence::ImageVerificationReport& verification,
    M3ReportContext context) {
    if (verification.depth != persistence::VerificationDepth::Structural &&
        verification.depth != persistence::VerificationDepth::Full) {
        return Result<M3Report>::failure(invalid_report(
            L"verification depth is unknown"));
    }
    M3Report report{};
    report.operation = verification.depth ==
            persistence::VerificationDepth::Full
        ? ReportOperation::VerifyFull : ReportOperation::VerifyStructural;
    report.status = verification.verified
        ? ReportStatus::Passed : ReportStatus::Failed;
    apply_context(report, std::move(context));
    apply_inspection(report, verification.inspection, "verify");
    report.verification = VerificationEvidence{
        verification.depth == persistence::VerificationDepth::Full
            ? VerificationDepth::Full : VerificationDepth::Structural,
        verification.verified, verification.candidate_blocks,
        verification.data_blocks, verification.zero_blocks,
        verification.data_bytes_hashed, verification.logical_sha256};
    if (verification.failure.has_value()) {
        report.error = error_evidence(*verification.failure, "verify");
    }
    if (!verification.verified && !report.error.has_value()) {
        return Result<M3Report>::failure(invalid_report(
            L"failed verification has no provenance"));
    }
    return Result<M3Report>::success(std::move(report));
}

Result<std::string> serialize_m3_report(const M3Report& report) {
    const auto valid = validate(report);
    if (!valid.ok()) return Result<std::string>::failure(valid.error());

    Json json;
    json.object_begin();
    json.key("schema_version"); json.unsigned_value(report.schema_version);
    json.key("operation"); json.string(*operation_name(report.operation));
    json.key("status"); json.string(*report_status_name(report.status));
    json.key("operation_id"); json.string(report.operation_id);
    json.key("started_utc"); json.string(report.started_utc);
    json.key("finished_utc"); json.string(report.finished_utc);
    json.key("build"); json.object_begin();
    json.key("source_commit"); json.string(report.build.source_commit);
    json.key("build_type"); json.string(report.build.build_type);
    json.key("binary_sha256"); json.string(report.build.binary_sha256);
    json.object_end();
    json.key("bundle_path_hash"); json.string(report.bundle_path_hash);
    json.key("bundle_path");
    if (report.include_absolute_path) json.string(report.bundle_path);
    else json.null();
    json.key("image");
    if (report.image.has_value()) {
        json.object_begin();
        json.key("image_uuid"); json.string(report.image->image_uuid);
        json.key("virtual_bytes"); json.unsigned_value(report.image->virtual_bytes);
        json.key("logical_sector_size");
        json.unsigned_value(report.image->logical_sector_size);
        json.key("physical_sector_size");
        json.unsigned_value(report.image->physical_sector_size);
        json.object_end();
    } else json.null();
    json.key("slots"); json.array_begin();
    for (const auto& slot : report.slots) {
        json.object_begin();
        json.key("slot"); json.string(slot.slot);
        json.key("status"); json.string(*slot_status_name(slot.status));
        json.key("generation"); optional_uint(json, slot.generation);
        json.key("first_failed_check");
        optional_string(json, slot.first_failed_check);
        json.key("check_status"); json.string(slot.check_status);
        json.key("selected"); json.boolean(slot.selected);
        json.object_end();
    }
    json.array_end();
    json.key("selected");
    if (report.selected.has_value()) {
        const auto& selected = *report.selected;
        json.object_begin();
        json.key("slot_index"); json.unsigned_value(selected.slot_index);
        json.key("fallback"); json.boolean(selected.fallback);
        json.key("checkpoint_generation");
        json.unsigned_value(selected.checkpoint_generation);
        json.key("base_generation");
        json.unsigned_value(selected.base_generation);
        json.key("journal_epoch"); json.unsigned_value(selected.journal_epoch);
        json.key("base_windows_identity");
        json.string(selected.base_windows_identity);
        json.key("base_format_identity");
        json.string(selected.base_format_identity);
        json.key("journal_windows_identity");
        json.string(selected.journal_windows_identity);
        json.key("journal_format_identity");
        json.string(selected.journal_format_identity);
        json.key("journal_committed_length");
        json.unsigned_value(selected.journal_committed_length);
        json.key("root"); json.object_begin();
        json.key("offset"); json.unsigned_value(selected.root.offset);
        json.key("length"); json.unsigned_value(selected.root.length);
        json.key("crc32c"); json.unsigned_value(selected.root.crc32c);
        json.object_end();
        json.key("commit"); json.object_begin();
        json.key("offset"); json.unsigned_value(selected.commit.offset);
        json.key("length"); json.unsigned_value(selected.commit.length);
        json.key("crc32c"); json.unsigned_value(selected.commit.crc32c);
        json.key("chain_sha256"); json.string(selected.commit.chain_sha256);
        json.object_end();
        json.object_end();
    } else json.null();
    json.key("verification");
    if (report.verification.has_value()) {
        const auto& verification = *report.verification;
        json.object_begin();
        json.key("depth"); json.string(*depth_name(verification.depth));
        json.key("verified"); json.boolean(verification.verified);
        json.key("candidate_blocks");
        json.unsigned_value(verification.candidate_blocks);
        json.key("data_blocks"); json.unsigned_value(verification.data_blocks);
        json.key("zero_blocks"); json.unsigned_value(verification.zero_blocks);
        json.key("data_bytes_hashed");
        json.unsigned_value(verification.data_bytes_hashed);
        json.key("logical_sha256");
        if (verification.logical_sha256.empty()) json.null();
        else json.string(verification.logical_sha256);
        json.object_end();
    } else json.null();
    json.key("cleanup_candidates"); json.array_begin();
    for (const auto& candidate : report.cleanup_candidates) {
        json.object_begin();
        json.key("kind"); json.string(candidate.kind);
        json.key("relative_name"); json.string(candidate.relative_name);
        json.key("decision"); json.string(candidate.decision);
        json.key("action"); json.string(candidate.action);
        json.object_end();
    }
    json.array_end();
    json.key("artifacts"); json.array_begin();
    for (const auto& artifact : report.artifacts) {
        json.object_begin();
        json.key("role"); json.string(artifact.role);
        json.key("sha256"); json.string(artifact.sha256);
        json.object_end();
    }
    json.array_end();
    json.key("error");
    if (report.error.has_value()) {
        json.object_begin();
        json.key("domain"); json.string(report.error->domain);
        json.key("code"); json.unsigned_value(report.error->code);
        json.key("native_code"); json.unsigned_value(report.error->native_code);
        json.key("phase"); json.string(report.error->phase);
        json.key("stable_code"); json.string(report.error->stable_code);
        json.object_end();
    } else json.null();
    json.object_end();
    return Result<std::string>::success(json.finish());
}

} // namespace myrd::m3
