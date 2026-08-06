#pragma once

#include "src/common/operation_context.h"

#include <filesystem>
#include <optional>
#include <string_view>
#if defined(MYRD_ENABLE_TEST_SEAMS)
#include <functional>
#endif

namespace myrd::m3 {

enum class EvidencePublishPhase : std::uint8_t {
    AfterTemporaryCreate,
    AfterExactWrite,
    AfterFlush,
    BeforeRename,
};

struct EvidencePublishOptions final {
#if defined(MYRD_ENABLE_TEST_SEAMS)
    std::function<Result<void>(EvidencePublishPhase)> phase_hook{};
#endif
};

struct EvidencePublishOutcome final {
    bool published{};
    std::filesystem::path published_path{};
    std::optional<std::filesystem::path> retained_temporary_path{};
    std::optional<Error> error{};
};

// Publishes a new evidence object only. The unique temporary file is always in
// the final file's directory and is intentionally retained on every failure so
// a half-written report can never masquerade as committed evidence.
[[nodiscard]] EvidencePublishOutcome publish_m3_evidence(
    const OperationContext& context,
    const std::filesystem::path& final_path,
    std::string_view exact_json,
    const EvidencePublishOptions& options);

} // namespace myrd::m3
