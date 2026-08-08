#pragma once

#include "src/common/result.h"
#include "src/persistence/image_inspector.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace myrd::m3 {

inline constexpr std::uint32_t kM3ReportSchemaVersion = 1;

enum class ReportOperation : std::uint8_t {
    Inspect,
    VerifyStructural,
    VerifyFull,
};

enum class ReportStatus : std::uint8_t {
    Passed,
    Failed,
    Blocked,
};

enum class SlotStatus : std::uint8_t {
    Empty,
    Selected,
    ValidNotSelected,
    FallbackRejected,
    Rejected,
};

enum class VerificationDepth : std::uint8_t {
    Structural,
    Full,
};

struct BuildEvidence final {
    std::string source_commit{};
    std::string build_type{};
    std::string binary_sha256{};
};

struct ImageEvidence final {
    std::string image_uuid{};
    std::uint64_t virtual_bytes{};
    std::uint32_t logical_sector_size{};
    std::uint32_t physical_sector_size{};
};

struct SlotEvidence final {
    std::string slot{};
    SlotStatus status{SlotStatus::Empty};
    std::optional<std::uint64_t> generation{};
    std::optional<std::string> first_failed_check{};
    std::string check_status{};
    bool selected{};
};

struct DescriptorEvidence final {
    std::uint64_t offset{};
    std::uint64_t length{};
    std::uint32_t crc32c{};
};

struct CommitEvidence final {
    std::uint64_t offset{};
    std::uint64_t length{};
    std::uint32_t crc32c{};
    std::string chain_sha256{};
};

struct SelectedGenerationEvidence final {
    std::uint8_t slot_index{};
    bool fallback{};
    std::uint64_t checkpoint_generation{};
    std::uint64_t base_generation{};
    std::uint64_t journal_epoch{};
    std::string base_windows_identity{};
    std::string base_format_identity{};
    std::string journal_windows_identity{};
    std::string journal_format_identity{};
    std::uint64_t journal_committed_length{};
    DescriptorEvidence root{};
    CommitEvidence commit{};
};

struct VerificationEvidence final {
    VerificationDepth depth{VerificationDepth::Structural};
    bool verified{};
    std::uint64_t candidate_blocks{};
    std::uint64_t data_blocks{};
    std::uint64_t zero_blocks{};
    std::uint64_t data_bytes_hashed{};
    std::string logical_sha256{};
};

struct CleanupCandidateEvidence final {
    std::string kind{};
    std::string relative_name{};
    std::string decision{};
    std::string action{};
};

struct ArtifactEvidence final {
    std::string role{};
    std::string sha256{};
};

struct ErrorEvidence final {
    std::string domain{};
    std::uint32_t code{};
    std::uint32_t native_code{};
    std::string phase{};
    std::string stable_code{};
    std::string operation{};
    std::optional<std::string> image_uuid{};
    std::string operation_id{};
    std::optional<std::uint64_t> generation{};
    persistence::CommitDisposition disposition{
        persistence::CommitDisposition::NotCommitted};
};

struct M3Report final {
    std::uint32_t schema_version{kM3ReportSchemaVersion};
    ReportOperation operation{ReportOperation::Inspect};
    ReportStatus status{ReportStatus::Failed};
    std::string operation_id{};
    std::string started_utc{};
    std::string finished_utc{};
    BuildEvidence build{};
    std::string bundle_path_hash{};
    std::string bundle_path{};
    bool include_absolute_path{};
    std::optional<ImageEvidence> image{};
    std::array<SlotEvidence, 2> slots{};
    std::optional<SelectedGenerationEvidence> selected{};
    std::optional<VerificationEvidence> verification{};
    std::vector<CleanupCandidateEvidence> cleanup_candidates{};
    std::vector<ArtifactEvidence> artifacts{};
    std::optional<ErrorEvidence> error{};
};

struct M3ReportContext final {
    std::string operation_id{};
    std::string started_utc{};
    std::string finished_utc{};
    BuildEvidence build{};
    std::string bundle_path_hash{};
    std::string bundle_path{};
    bool include_absolute_path{};
    std::vector<CleanupCandidateEvidence> cleanup_candidates{};
    std::vector<ArtifactEvidence> artifacts{};
};

[[nodiscard]] Result<M3Report> make_m3_report(
    const persistence::ImageInspection& inspection,
    M3ReportContext context);

[[nodiscard]] Result<M3Report> make_m3_report(
    const persistence::ImageVerificationReport& verification,
    M3ReportContext context);

[[nodiscard]] Result<std::string> serialize_m3_report(
    const M3Report& report);

} // namespace myrd::m3
