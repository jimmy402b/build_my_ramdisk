#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace myrd::persistence::test {

enum class ReferenceManifestCheck : std::uint8_t {
    SlotDecode, SlotSupport, SlotSemantics, BasePath, JournalPath,
    BasePresence, BaseOpen, BaseIdentity, BaseExpectedLength, BaseProfile,
    BaseLayout, JournalPresence, JournalOpen, JournalIdentity, JournalHeader,
    JournalCommittedLength, JournalCommit, JournalRoot, JournalDigest, Count,
};
inline constexpr std::size_t kReferenceManifestCheckCount =
    static_cast<std::size_t>(ReferenceManifestCheck::Count);

enum class ReferenceManifestStatus : std::uint8_t {
    NotRun, Passed, Missing, Corrupt, Unsupported, AccessDenied, IoError,
    Cancelled, DeadlineExceeded, Conflict,
};

enum class ReferenceMalformedKind : std::uint8_t {
    None, SlotOutOfRange, DuplicateSlot, MissingDecodedSlot,
    TrustStateMismatch, TrustedGenerationMismatch,
};

struct ReferenceManifestCandidate final {
    std::uint8_t slot_index{};
    std::optional<std::uint64_t> trusted_generation{};
    std::string durable_key{};
    std::array<ReferenceManifestStatus,
               kReferenceManifestCheckCount> checks{};
    std::array<std::string, kReferenceManifestCheckCount> provenance{};
    ReferenceMalformedKind malformed{ReferenceMalformedKind::None};
};

enum class ReferenceManifestSelectionKind : std::uint8_t {
    Selected, NoCompleteCandidate, EqualGenerationConflict, InvalidEvidence,
};

struct ReferenceManifestSlotVerdict final {
    std::uint8_t slot_index{};
    bool complete{};
    std::optional<std::uint64_t> trusted_generation{};
    std::optional<ReferenceManifestCheck> first_rejection{};
    ReferenceManifestStatus status{ReferenceManifestStatus::NotRun};
    std::string provenance{};
};

struct ReferenceManifestSelection final {
    ReferenceManifestSelectionKind kind{};
    std::optional<std::uint8_t> selected_slot{};
    std::optional<std::uint64_t> selected_generation{};
    bool fallback{};
    ReferenceMalformedKind malformed{ReferenceMalformedKind::None};
    std::array<ReferenceManifestSlotVerdict, 2> diagnostics{};
};

ReferenceManifestSelection reference_select_manifest(
    const std::array<ReferenceManifestCandidate, 2>& candidates);

}  // namespace myrd::persistence::test
