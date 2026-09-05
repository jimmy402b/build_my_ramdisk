#pragma once

#include "src/common/operation_context.h"
#include "src/m3/m3_cli.h"
#include "src/persistence/persistence_types.h"

#include <cstdint>
#include <optional>
#include <string>

namespace myrd::m3 {

struct M3LifecycleOutcome final {
    int exit_code{1};
    persistence::CommitDisposition disposition{
        persistence::CommitDisposition::NotCommitted};
    std::uint64_t generation{};
    bool cleanup_succeeded{true};
    std::optional<Error> error{};
    std::string json{};
    std::string diagnostics{};
};

// The runner is the only M3 CLI composition root. It owns no driver/proxy
// objects and routes image operations through the persistence/session APIs.
[[nodiscard]] M3LifecycleOutcome run_m3_lifecycle(
    const OperationContext& context,
    const M3CliConfig& config);

} // namespace myrd::m3
